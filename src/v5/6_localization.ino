// -----Localization (dead reckoning) + route memory-----
// A drift-free heading and a wheel encoder are, together, enough to know where
// the robot is on the mat, not just which way it's pointing. So we keep a live
// (x, y) position in cm by integrating how far the wheels rolled along the
// current heading, and we drop a breadcrumb every so often so we can rebuild
// the exact line we drove. The corner lines the camera already finds are fixed
// features of the mat, so we mark them and use them to keep the estimate honest.

#define TRAIL_STEP_CM 1.5   // how often we record a point of the path
#define MAX_PATH 1200       // breadcrumb buffer size
#define CORNER_DEDUP_CM 20  // don't mark the same corner twice

struct PathPt {
  float x;
  float y;
  float heading;
  uint8_t tag;        // 0 = plain breadcrumb, 1 = corner mark
};

PathPt path[MAX_PATH];
int path_len = 0;

double robot_x = 0, robot_y = 0;        // position on the mat, in cm
double last_odo_cm = 0;                 // encoder distance at the last update
double last_log_x = 0, last_log_y = 0;  // where we dropped the last breadcrumb
double last_corner_x = -1000, last_corner_y = -1000;

void localization_reset() {
  robot_x = robot_y = 0;
  last_odo_cm = read_motor_cm();
  last_log_x = last_log_y = 0;
  last_corner_x = last_corner_y = -1000;
  path_len = 0;
}

// integrate one step of dead reckoning: take the distance the wheels rolled
// since the last call and project it onto the current heading
void update_odometry() {
  double cm = read_motor_cm();
  double dd = cm - last_odo_cm;
  last_odo_cm = cm;
  if (dd == 0)
    return;

  double th = gx * DEG_TO_RAD; // gx is our drift-free heading, in degrees
  robot_x += dd * sin(th);
  robot_y += dd * cos(th);

  // drop a breadcrumb every TRAIL_STEP_CM so we can replay the line later
  double ex = robot_x - last_log_x, ey = robot_y - last_log_y;
  if (ex * ex + ey * ey >= TRAIL_STEP_CM * TRAIL_STEP_CM && path_len < MAX_PATH) {
    path[path_len++] = { (float)robot_x, (float)robot_y, (float)gx, 0 };
    last_log_x = robot_x;
    last_log_y = robot_y;
  }
}

// tag the current position as a corner (camera sends 'M' when it sees a turn
// line). de-duped so the repeated triggers from consecutive frames only mark
// the corner once. these anchors are what we lean on to stop the lateral drift
// of the dead-reckoning from piling up over a whole run.
void mark_corner() {
  double dx = robot_x - last_corner_x, dy = robot_y - last_corner_y;
  if (dx * dx + dy * dy < CORNER_DEDUP_CM * CORNER_DEDUP_CM)
    return; // we already marked this one
  last_corner_x = robot_x;
  last_corner_y = robot_y;
  if (path_len < MAX_PATH)
    path[path_len++] = { (float)robot_x, (float)robot_y, (float)gx, 1 };
}
