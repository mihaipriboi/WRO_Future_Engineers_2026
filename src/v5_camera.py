import sensor
import time
from machine import UART, LED
import network
import socket
import image

# setup UART connection to arduino
uart = UART(1, 19200, timeout_char=200)
# 1 - the uart config, meaning that we use pins P4 as the transmitter, P5 as the receiver
# 19200 - baud rate aka frequency, must match the one set up on the arduino

# initialize the sensor
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.set_vflip(True)
sensor.set_hmirror(True)

# skip some frames to let the camera adjust
sensor.skip_frames(time=3000)

# manually configure settings
rgb_gain_db = sensor.get_rgb_gain_db()
print(rgb_gain_db)
sensor.set_auto_whitebal(False, rgb_gain_db=(61.4, 60.2, 65)) # (62, 60, 64)
sensor.set_auto_exposure(False, exposure_us=10000)
sensor.set_auto_gain(False, gain_db=17) # 10

# network setup
SSID = "OPENMV_AP"  # Network SSID
KEY = "1234567890"  # Network key (must be 10 chars)
HOST = ""  # Use first available interface
PORT = 8080  # Arbitrary non-privileged port

led = LED('LED_GREEN')
led.on()
time.sleep(0.5)
led.off()

DEBUG_STREAMING = False
streaming_initialized = False
client = None
streaming = False
connection_time_max = 120
server = None

if DEBUG_STREAMING:
    led = LED('LED_GREEN')
    led.on()
    time.sleep(0.5)
    led.off()

    # Init wlan module in AP mode.
    wlan = network.WLAN(network.AP_IF)
    wlan.active(False)
    wlan.config(ssid=SSID, key=KEY, channel=2)
    wlan.active(True)

    print("AP mode started. SSID: {} IP: {}".format(SSID, wlan.ifconfig()[0]))

    # Setup server and wait until a client connects
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, True)
    server.bind([HOST, PORT])
    server.listen(1)
    print("Waiting for client to connect on port", PORT)

    while True:
        try:
            client, addr = server.accept()
            client.settimeout(5.0)
            print("Client connected from", addr)
            led = LED('LED_GREEN')
            led.on()
            time.sleep(0.5)
            led.off()
            streaming = True
            break
        except OSError:
            time.sleep(0.1)


main_clock = time.clock()
fps = 0

def start_streaming_step(client, frame):
    global streaming_initialized, main_clock, fps

    # If we haven't sent the MJPEG header yet, try now
    if not streaming_initialized:
        try:
            # Non-blocking read to flush client's request
            client.settimeout(0.01)
            try:
                client.recv(1024)
            except:
                pass  # Ignore timeout or no data
            client.settimeout(1.0)

            client.send(
                "HTTP/1.1 200 OK\r\n"
                "Server: OpenMV\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=openmv\r\n"
                "Cache-Control: no-cache\r\n"
                "Pragma: no-cache\r\n\r\n"
            )
            streaming_initialized = True
            main_clock.reset()
            print("Streaming initialized.")
        except OSError as e:
            print("Failed to send header:", e)
            try: client.close()
            except: pass
            return None  # client lost, clear it

    # Try sending a frame
    try:
        main_clock.tick()
        cframe = frame.compress(quality=20, subsampling=image.JPEG_SUBSAMPLING_422)
        header = (
            "\r\n--openmv\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: {}\r\n\r\n".format(cframe.size())
        )
        client.sendall(header)
        client.sendall(cframe)
        fps = main_clock.fps()
        return client  # all good
    except OSError as e:
        print("Client disconnected:", e)
        try: client.close()
        except: pass
        streaming_initialized = False
        return None  # client disconnected


sensor.skip_frames(time=3000)

outside_block_roi = [(0, 0, 320, 80)]

# logic
direction = 0
FINAL = True

if FINAL:
    led = LED('LED_RED')
    led.on()
    time.sleep(0.5)
    led.off()
    # blink led twice so that we know the camera is ready
else:
    led = LED('LED_BLUE')
    led.on()
    time.sleep(0.5)
    led.off()
    # blink led once so that we know the camera is ready

# threshold values
red_threshold = [(25, 65, 10, 55, -5, 45)]

green_threshold = [(25, 65, -45, -15, -5, 35)]

blue_threshold = [(20, 80, -15, 10, -30, -10)]

orange_threshold = [(45, 85, 0, 35, 0, 45)]

# parking_threshold = [(30, 70, 10, 60, -15, 15)]
parking_threshold = []

black_threshold = [(0, 45, -10, 15, -25, 10)]

# colours are tuned in the pit over WiFi (drag a box on the live image) and
# saved to thresholds.json on the cam; the race code just loads whatever we
# last calibrated, and falls back to the defaults above if the file isn't there
def load_thresholds():
    global red_threshold, green_threshold, blue_threshold, orange_threshold
    try:
        import json
        with open("thresholds.json") as f:
            t = json.load(f)
        red_threshold    = [tuple(t["red"])]
        green_threshold  = [tuple(t["green"])]
        blue_threshold   = [tuple(t["blue"])]
        orange_threshold = [tuple(t["orange"])]
        print("loaded tuned thresholds from thresholds.json")
    except (OSError, ValueError, KeyError):
        print("no thresholds.json found, using built-in defaults")

load_thresholds()

# define threshold for "darkness"
dark_threshold = 50
darkness_limit = 85  # 85% threshold for too dark

# ROI values
img = sensor.snapshot()
cubes_roi = (0, int(img.height() / 2 - 32), img.width(), int(img.height() / 2 - 16))
lines_roi = (0, int(img.height() / 3 + 30), img.width(), int(img.height() * 2 / 3 - 30))
#parking_roi = (0, 112, img.width(), 40)
parking_roi = (0, int(img.height() / 2 + 16), img.width(), int(img.height() / 2 - 16))
wall_roi = (0, int(img.height() / 3 + 20), img.width(), int(img.height() * 2 / 3 - 20))
wall_roi_area = wall_roi[2] * wall_roi[3]

wall_left_roi = (0, 100, 120, 60)
wall_right_roi = (200, 100, 120, 60)

wall_dark_roi = (80, 100, 160, 80)

# restrains values
min_cube_height = 12
min_cube_size = 180
#max_cube_size_red = 1560 # 1600
#max_cube_size_green = 1000 # 1200
max_cube_height_red = 55
max_cube_height_green = 55
wall_blob_size = 18200

line_blob_size = 500
parking_blob_height_trigger = 20
parking_blob_size_trigger = 2800
parking_blob_size_min = 140
density_thr = 0.7 # 0.6

# PID values
kp = 0.00095
kd = 0.0045
err_old = 0

def send_message(msg):
    uart.write(msg + '\n')
    # print(msg + '\n')

# force the val into the [min_intv, max_intv] interval
def clamp(val, min_intv, max_intv):
    if val < min_intv:
        return min_intv
    if val > max_intv:
        return max_intv
    return round(val, 3)

# detects if a blob is partially inside another blob
# by checking if the center of one blob is inside the minimum area rectangle that wraps the other blob
# giving the fact that this rectangle may be crooked, we have to use a special algorithm
# that is designed to check whether a point is inside a polygon or not based on the coordinates
def is_blob_in_blob(blob, blob2):
    if not blob2:
        return False
    polygon = blob2.min_corners()
    num_vertices = len(polygon)
    (x, y) = (blob.cx(), blob.cy())
    inside = False

    # store the first point in the polygon
    (p1x, p1y) = (polygon[0][0], polygon[0][1])

    # loop through each edge in the polygon
    for i in range(1, num_vertices + 1):
        # get the next point in the polygon
        (p2x, p2y) = (polygon[i % num_vertices][0], polygon[i % num_vertices][1])

        # check if the point is above the minimum y coordinate of the edge
        # check if the point is below the maximum y coordinate of the edge
        # check if the point is to the left of the maximum x coordinate of the edge
        if y > min(p1y, p2y) and y <= max(p1y, p2y) and x <= max(p1x, p2x):
            # calculate the x-intersection of the line connecting the point to the edge
            x_intersection = (y - p1y) * (p2x - p1x) / (p2y - p1y) + p1x

            # check if the point is on the same line as the edge or to the left of the x-intersection
            if p1x == p2x or x <= x_intersection:
                # flip the inside flag
                inside = not inside

        # store the current point as the first point for the next iteration
        (p1x, p1y) = (p2x, p2y)

    # return the value of the inside flag
    return inside

# checks whether a blob is a cube or not based on the minimum height and density filters
# as well as another check:
# since in some light conditions the red the cube may be similar to the orange line or magenta parking walls
# and in some light conditions the green the cube may be similar to the blue line
# we have to check if the possible cube blob is inside any of these ones
# if it is, then it may just be a false alarm and we should ignore it
def is_cube(blob, line_blob, parking_blobs):
    if blob.density() < density_thr or blob.h() <= min_cube_height:
        return False
    for parking_wall_blob in parking_blobs:
        if is_blob_in_blob(blob, parking_wall_blob):
            return False
    if not is_blob_in_blob(blob, line_blob):
        return True
    return False

def is_line(blob, cube_blobs, parking_blobs):
    for parking_wall_blob in parking_blobs:
        if is_blob_in_blob(blob, parking_wall_blob):
            return False
    for cube_blob in cube_blobs:
        if is_blob_in_blob(blob, cube_blob):
            return False
    return True

# function that gets the biggest blob by size
def get_biggest_blob(blob_array):
    max_area = 0
    max_blob = None
    for blob in blob_array:
        # we're keeping the biggest parking blob
        if blob.area() > max_area:
            max_area = blob.area()
            max_blob = blob
    return max_blob

# checks if a parking wall blob meets the height and size requirements
def is_parking_wall(blob):
    if not blob:
        return False
    if blob.pixels() >= parking_blob_size_trigger and blob.area() >= parking_blob_size_trigger and blob.h() >= parking_blob_height_trigger:
        return True
    return False

while (True):
    img = sensor.snapshot()
    gray_img = img.copy()  # make a copy of the image
    gray_img.to_grayscale()  # convert to grayscale

    # determine the brightness of the ROIs to know if we're close to an outside wall or not
    left_dark_pixels = 0
    right_dark_pixels = 0
    middle_dark_pixels = 0

    # process pixels in the left ROI
    for y in range(wall_dark_roi[1], wall_dark_roi[1] + wall_dark_roi[3]):
        for x in range(wall_dark_roi[0], wall_dark_roi[0] + wall_dark_roi[2]):
            if gray_img.get_pixel(x, y) < dark_threshold:
                middle_dark_pixels += 1

    # process pixels in the left ROI
    for y in range(wall_left_roi[1], wall_left_roi[1] + wall_left_roi[3]):
        for x in range(wall_left_roi[0], wall_left_roi[0] + wall_left_roi[2]):
            if gray_img.get_pixel(x, y) < dark_threshold:
                left_dark_pixels += 1

    # process right ROI
    for y in range(wall_right_roi[1], wall_right_roi[1] + wall_right_roi[3]):
        for x in range(wall_right_roi[0], wall_right_roi[0] + wall_right_roi[2]):
            if gray_img.get_pixel(x, y) < dark_threshold:
                right_dark_pixels += 1


    # calculate darkness percentages
    left_total_pixels = wall_left_roi[2] * wall_left_roi[3]
    right_total_pixels = wall_right_roi[2] * wall_right_roi[3]
    middle_total_pixels = wall_dark_roi[2] * wall_dark_roi[3]

    left_dark_percentage = (left_dark_pixels / left_total_pixels) * 100
    right_dark_percentage = (right_dark_pixels / right_total_pixels) * 100
    middle_dark_percentage = (middle_dark_pixels / middle_total_pixels) * 100

#    img.draw_rectangle(wall_dark_roi, color=(0, 0, 255))  # Left ROI in blue
#    img.draw_string(wall_dark_roi[0], wall_dark_roi[1] - 10, str(middle_dark_percentage), color=(255, 0, 0))


#    # draw ROIs with blue outlines
#    img.draw_rectangle(wall_left_roi, color=(0, 0, 255))  # Left ROI in blue
#    img.draw_rectangle(wall_right_roi, color=(0, 0, 255))  # Right ROI in blue

#    # overlay the darkness percentages
#    img.draw_string(wall_left_roi[0], wall_left_roi[1] - 10, str(left_dark_percentage), color=(255, 0, 0))
#    img.draw_string(wall_right_roi[0], wall_right_roi[1] - 10, str(right_dark_percentage), color=(255, 0, 0))

    # find the coloured blobs corresponding to the turn lines
    orange_blobs = img.find_blobs(orange_threshold, roi=lines_roi, pixels_threshold=line_blob_size, area_threshold=line_blob_size, merge=True)
    blue_blobs = img.find_blobs(blue_threshold, roi=lines_roi, pixels_threshold=line_blob_size, area_threshold=line_blob_size, merge=True)

    # find the coloured blobs corresponding to the cubes
    red_blobs = img.find_blobs(red_threshold, roi=cubes_roi, pixels_threshold=min_cube_size, area_threshold=min_cube_size, merge=True)
    green_blobs = img.find_blobs(green_threshold, roi=cubes_roi, pixels_threshold=min_cube_size, area_threshold=min_cube_size, merge=True)

    # find the coloured blobs corresponding to the parking walls
    parking_blobs = img.find_blobs(parking_threshold, roi=parking_roi, pixels_threshold=parking_blob_size_min, area_threshold=parking_blob_size_min, merge=False)
    parking_wall_blob = get_biggest_blob(parking_blobs)

    orange_blob_w = None
    orange_blob_h = None
    max_width = 0
    max_height = 0
    for blob in orange_blobs:
        if is_line(blob, red_blobs, parking_blobs):
            if blob.w() >= img.width() * 0.4: # if it meets the minimum width requirement
                if blob.w() > max_width: # if it's the biggest blob yet
                    max_width = blob.w()
                    orange_blob_w = blob # biggest blob on the width
            if blob.h() >= lines_roi[3] * 0.4: # if it meets the minimum height requirement
                if blob.h() > max_height: # if it's the biggest blob yet
                    max_height = blob.h()
                    orange_blob_h = blob # biggest blob on the height
    # if we have a blob meeting either the minimum width or height requirement we remember it
    orange_blob = orange_blob_w
    if not orange_blob:
        orange_blob = orange_blob_h

    blue_blob_w = None
    blue_blob_h = None
    max_width = 0
    max_height = 0
    for blob in blue_blobs:
        if is_line(blob, green_blobs, parking_blobs):
            if blob.w() >= img.width() * 0.3: # if it meets the minimum width requirement
                if blob.w() > max_width: # if it's the biggest blob yet
                    max_width = blob.w()
                    blue_blob_w = blob # biggest blob on the width
            if blob.h() >= lines_roi[3] * 0.4: # if it meets the minimum height requirement
                if blob.h() > max_height: # if it's the biggest blob yet
                    max_height = blob.h()
                    blue_blob_h = blob # biggest blob on the height
    # if we have a blob meeting either the minimum width or height requirement we remember it
    blue_blob = blue_blob_w
    if not blue_blob:
        blue_blob = blue_blob_h

    # img.draw_rectangle(lines_roi, color=(0, 255, 0))
    # if orange_blob:
    #    img.draw_edges(orange_blob.min_corners(), color=(255, 0, 0))
    #    img.draw_line(orange_blob.major_axis_line(), color=(0, 255, 0))
    #    img.draw_line(orange_blob.minor_axis_line(), color=(0, 255, 0))
    # if blue_blob:
    #    img.draw_edges(blue_blob.min_corners(), color=(255, 0, 0))
    #    img.draw_line(blue_blob.major_axis_line(), color=(0, 0, 255))
    #    img.draw_line(blue_blob.minor_axis_line(), color=(0, 0, 255))

    if direction == 0: # if we didn't set a turn direction yet
        if orange_blob: # if the first line we saw was an orange one
            direction = 2
        elif blue_blob: # if the first line we saw was a blue one
            direction = 1

    has_line = False
    if orange_blob or blue_blob: # if we saw either coloured lines, we can make a turn
        has_line = True

    if FINAL: # if we're running the code for the final challenge
#        if parking_wall_blob:
#            img.draw_rectangle(parking_wall_blob.rect(), color=(0, 0, 255))  # RGB: Blue
#            # draw a cross at the center of the detected object
#            img.draw_cross(parking_wall_blob.cx(), parking_wall_blob.cy(), color=(0, 0, 255))  # RGB: Blue
#            # add text annotation with information about the object
#            img.draw_string(parking_wall_blob.cx() - 47, parking_wall_blob.cy() - 48,
#                            "Object: Parking",
#                            color=(0, 255, 0), scale=1)  # Adjust scale as needed
#            img.draw_string(parking_wall_blob.cx() - 42, parking_wall_blob.cy() - 38,
#                            "Area: {}".format(parking_wall_blob.pixels()),
#                            color=(0, 255, 0), scale=1)  # Adjust scale as needed
#            img.draw_string(parking_wall_blob.cx() - 47, parking_wall_blob.cy() - 28,
#                            "X: {}, Y: {}".format(parking_wall_blob.cx(), parking_wall_blob.cy()),
#                            color=(0, 255, 0), scale=1)
#        if parking_wall_blob:
#            img.draw_rectangle(parking_wall_blob.rect(), color=(0, 255, 0))

        # find the coloured blobs corresponding to the outside walls
        wall_blobs = img.find_blobs(black_threshold, roi=wall_roi, pixels_threshold=wall_blob_size, area_threshold=wall_blob_size, merge=True)
        outer_wall = get_biggest_blob(wall_blobs)
#        img.draw_rectangle(wall_roi, color=(0,0,255))
#        for blob in wall_blobs:
#            img.draw_rectangle(blob.rect(), color=(0, 255, 0))

        msg = "0"
        max_area = 0
        color = 'none'
        saved_cube = None
        for blob in red_blobs: # for every red blob
            # if they're passing the height and density filters
            # we're keeping the biggest one and its color
            if is_cube(blob, orange_blob, parking_blobs) and blob.area() > max_area:
                max_area = blob.area()
                saved_cube = blob
                color = 'red'
        for blob in green_blobs: # for every green blob
            # if they're passing the height and density filters
            # we're keeping the biggest one and its color
            if is_cube(blob, blue_blob, parking_blobs) and blob.area() > max_area:
                max_area = blob.area()
                saved_cube = blob
                color = 'green'

        # img.draw_rectangle(cubes_roi, color=(255, 0, 0))
        if saved_cube != None: # if we saw a cube
            # draw a blue rectangle around the detected object
            img.draw_rectangle(saved_cube.rect(), color=(0, 0, 255))
#            # draw a cross at the center of the detected object
#            # add text annotation with information about the object
#            img.draw_string(saved_cube.cx() - 47, saved_cube.cy() - 45,
#                            "Color: {}".format(color),
#                            color=(255, 0, 0), scale=1)  # Adjust scale as needed
#            img.draw_string(saved_cube.cx() - 42, saved_cube.cy() - 35,
#                            "Area: {}".format(saved_cube.pixels()),
#                            color=(255, 0, 0), scale=1)  # Adjust scale as needed
#            img.draw_string(saved_cube.cx() - 42, saved_cube.cy() - 55,
#                            "height: {}".format(saved_cube.h()),
#                            color=(255, 0, 0), scale=1)  # Adjust scale as needed
#            img.draw_string(saved_cube.cx() - 47, saved_cube.cy() - 25,
#                            "X: {}, Y: {}".format(saved_cube.cx(), saved_cube.cy()),
#                            color=(255, 0, 0), scale=1)  # Adjust scale as needed
            # img.draw_rectangle(saved_cube.rect())
            # img.draw_cross(saved_cube.cx(), saved_cube.cy())

            # if the cube area is over a certain threshold
            # it means we must avoid the cube as we are too close to it
            if (color == 'red' and saved_cube.h() >= max_cube_height_red) or (color == 'green' and saved_cube.h() >= max_cube_height_green):
                img.draw_cross(saved_cube.cx(), saved_cube.cy(), color=(0, 0, 255))
                # send the right trigger
                err_old = 0
                if color == 'red':
                    send_message('R')
#                    print('R')
                else:
                    send_message('G')
#                    print('G')
                if has_line:
                    # if we must also turn, send the trigger
                    send_message(str(direction)) # send the turn trigger
#                    print(str(direction))
            else: # if the cube isn't too big we must follow it
                # calculate the angle using PID

#                if color == 'red':
                err = saved_cube.cx() - (img.width() / 2)
#                else:
#                    err = saved_cube.cx() - (img.width() / 2 - 5)


#                if left_dark_percentage > darkness_limit and saved_cube.pixels() <= 250:
#                    steering = -0.35 # too dark on the left, steer right
##                    print("debug")
#                elif right_dark_percentage > darkness_limit and saved_cube.pixels() <= 250:
#                    steering = 0.35
##                    print("debug")
#                else:
                steering = err * kp + (err - err_old) * kd
                steering = -clamp(steering, -1, 1)
                err_old = err
                # craft the command
                # adjust error based on darkness levels
                if color == 'red':
                    msg = 'r' + str(steering)
                else:
                    msg = 'g' + str(steering)
                send_message(msg) # send the message
                print(msg)
                if has_line:
                    # if we must also turn, send the turn trigger
                    send_message(str(direction))
#                    print(str(direction))
        elif has_line: # if we don't see any cubes
            # if we must turn, send the turn trigger
            send_message(str(direction))
            send_message('M') # tag this corner on the robot's map
#            print(str(direction))
        if is_parking_wall(parking_wall_blob):
            # if we saw the parking walls, send the parking trigger
#            img.draw_rectangle(parking_wall_blob.rect(), color=(0, 255, 0))
            send_message('P')
#            print('P')
        if wall_blobs:
            # if the wall is big enough, send a slightly different message that helps us when parking
            # if not, send the classic one
            if middle_dark_percentage > 75:
                send_message('WP')
#                print('WP')
            else:
                send_message('W')
#                print('W')
    else: # if we're running the quali code
        if has_line:
            # if we must turn, send the turn trigger
            send_message(str(direction))
            send_message('M') # tag this corner on the robot's map
#            print(str(direction))

    if DEBUG_STREAMING:
        # Try accepting a new client if we don't have one
        # Try accepting a new client if none is connected
        if client is None:
            try:
                client, addr = server.accept()
                client.settimeout(2.0)
                print("Client connected from", addr)
            except OSError:
                pass  # no new client

    # If a client is connected, try sending a frame
    if client:
        img.draw_string(10, 20, str(int(fps)), color=(255, 255, 0), scale=2)
        client = start_streaming_step(client, img)
