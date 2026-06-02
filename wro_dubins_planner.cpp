/*
 * WRO Future Engineers 2025 – Obstacle Challenge Path Planner
 * ============================================================
 * Calculeaza traseul optim (cel mai scurt) pentru un robot cu raza
 * minima de viraj (Dubins Car), evitand pilonii rosii/verzi conform regulilor:
 *   - Pilon ROSU  → treci pe DREAPTA pilonului
 *   - Pilon VERDE → treci pe STANGA  pilonului
 *
 * Algoritmul:
 *   1. Modeleaza terenul WRO (inel patrat, 3 tururi)
 *   2. Genereaza waypoint-uri laterale pentru fiecare pilon
 *   3. Conecteaza waypoint-urile prin segmente Dubins (CSC / CCC)
 *   4. Alege combinatia de arce cu lungimea totala minima
 *
 * Compilare:
 *   g++ -std=c++17 -O2 -o wro_planner wro_dubins_planner.cpp -lm
 *
 * Output:
 *   - Lista waypoint-uri cu coordonate (x, y, heading)
 *   - Lungimea totala a traseului
 *   - Tipul fiecarui segment Dubins (LSL, RSR, LSR, RSL, RLR, LRL)
 */

#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <iomanip>
#include <limits>
#include <cassert>

// ─────────────────────────────────────────────────────────────
//  CONSTANTE TEREN  (mm, conform regulament WRO 2025)
// ─────────────────────────────────────────────────────────────
static constexpr double TRACK_OUTER  = 1000.0; // latimea exterioara a terenului (mm)
static constexpr double TRACK_INNER  =  600.0; // latimea zonei interioare
static constexpr double LANE_WIDTH   = (TRACK_OUTER - TRACK_INNER) / 2.0; // ~200 mm
static constexpr double PILLAR_RADIUS =  25.0; // raza fizica a pilonului (mm)

// Raza minima de viraj a robotului (ajusteaza pentru robotul tau)
static constexpr double MIN_TURN_RADIUS = 175.0; // mm

// Offset lateral fata de pilon (cat de departe treci pe langa el)
static constexpr double PASS_OFFSET = PILLAR_RADIUS + 80.0; // mm

// ─────────────────────────────────────────────────────────────
//  TIPURI DE DATE
// ─────────────────────────────────────────────────────────────

struct Vec2 {
    double x, y;
    Vec2(double x=0, double y=0) : x(x), y(y) {}
    Vec2 operator+(const Vec2& o) const { return {x+o.x, y+o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x-o.x, y-o.y}; }
    Vec2 operator*(double s)      const { return {x*s, y*s}; }
    double dot(const Vec2& o)     const { return x*o.x + y*o.y; }
    double norm()                 const { return std::sqrt(x*x + y*y); }
    Vec2 normalized()             const { double n=norm(); return n>1e-9?Vec2{x/n,y/n}:Vec2{0,0}; }
    Vec2 perp()                   const { return {-y, x}; } // rotatie 90° CCW
};

// Starea completa a vehiculului (pozitie + directie)
struct Pose {
    double x, y;
    double theta; // radiani, 0 = Est, pi/2 = Nord

    Pose(double x=0, double y=0, double theta=0) : x(x), y(y), theta(theta) {}

    Vec2 pos()      const { return {x, y}; }
    Vec2 forward()  const { return {std::cos(theta), std::sin(theta)}; }
};

// Culoarea pilonului
enum class PillarColor { RED, GREEN };

// Pilon pe teren
struct Pillar {
    Vec2        pos;
    PillarColor color;
    int         id;

    Pillar(double x, double y, PillarColor c, int id)
        : pos(x, y), color(c), id(id) {}
};

// Tip segment Dubins
enum class DubinsType { LSL, RSR, LSR, RSL, RLR, LRL, INVALID };

inline const char* dubinsTypeName(DubinsType t) {
    switch(t) {
        case DubinsType::LSL: return "LSL";
        case DubinsType::RSR: return "RSR";
        case DubinsType::LSR: return "LSR";
        case DubinsType::RSL: return "RSL";
        case DubinsType::RLR: return "RLR";
        case DubinsType::LRL: return "LRL";
        default:             return "INVALID";
    }
}

// Un segment Dubins complet (3 sub-segmente: arc1, linie, arc2)
struct DubinsPath {
    DubinsType type   = DubinsType::INVALID;
    double     t      = 0; // lungime arc1 (radiani * r)
    double     p      = 0; // lungime segment drept
    double     q      = 0; // lungime arc2 (radiani * r)
    double     r      = MIN_TURN_RADIUS;
    double     length = std::numeric_limits<double>::infinity();
    Pose       start, end;
};

// ─────────────────────────────────────────────────────────────
//  UTILITARE UNGHIURI
// ─────────────────────────────────────────────────────────────

static double mod2pi(double angle) {
    while (angle <  0)       angle += 2*M_PI;
    while (angle >= 2*M_PI)  angle -= 2*M_PI;
    return angle;
}

static double angleDiff(double from, double to) {
    // Diferenta minima intre doua unghiuri (-pi, pi]
    double d = mod2pi(to - from);
    if (d > M_PI) d -= 2*M_PI;
    return d;
}

// ─────────────────────────────────────────────────────────────
//  DUBINS PATH  –  cele 6 tipuri
//  Referinta: Shkel & Lumelsky (2001) / Lavalle (2006)
// ─────────────────────────────────────────────────────────────

// Centru cerc stanga (CCW) / dreapta (CW)
static Vec2 circleCenter(const Pose& p, double r, bool left) {
    double angle = p.theta + (left ? M_PI/2 : -M_PI/2);
    return { p.x + r * std::cos(angle), p.y + r * std::sin(angle) };
}

static double dubinsLSL(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cL1 = circleCenter(s, r, true);
    Vec2 cL2 = circleCenter(e, r, true);
    Vec2 d   = cL2 - cL1;
    double D = d.norm();
    if (D < 1e-9) return std::numeric_limits<double>::infinity();

    double alpha = std::atan2(d.y, d.x);
    t = mod2pi(alpha - s.theta + M_PI/2);
    q = mod2pi(e.theta - alpha + M_PI/2);
    p_out = D;
    return r*(t + q) + p_out;
}

static double dubinsRSR(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cR1 = circleCenter(s, r, false);
    Vec2 cR2 = circleCenter(e, r, false);
    Vec2 d   = cR2 - cR1;
    double D = d.norm();
    if (D < 1e-9) return std::numeric_limits<double>::infinity();

    double alpha = std::atan2(d.y, d.x);
    t = mod2pi(s.theta - alpha + M_PI/2);
    q = mod2pi(alpha - e.theta + M_PI/2);
    p_out = D;
    return r*(t + q) + p_out;
}

static double dubinsLSR(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cL1 = circleCenter(s, r, true);
    Vec2 cR2 = circleCenter(e, r, false);
    Vec2 d   = cR2 - cL1;
    double D2 = d.dot(d);
    if (D2 < 4*r*r) return std::numeric_limits<double>::infinity();

    double D    = std::sqrt(D2);
    double phi  = std::atan2(d.y, d.x);
    double beta = std::acos(2*r / D);
    t     = mod2pi(phi + beta - s.theta - M_PI/2);
    q     = mod2pi(phi + beta - e.theta - M_PI/2);
    p_out = std::sqrt(D2 - 4*r*r);
    return r*(t + q) + p_out;
}

static double dubinsRSL(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cR1 = circleCenter(s, r, false);
    Vec2 cL2 = circleCenter(e, r, true);
    Vec2 d   = cL2 - cR1;
    double D2 = d.dot(d);
    if (D2 < 4*r*r) return std::numeric_limits<double>::infinity();

    double D    = std::sqrt(D2);
    double phi  = std::atan2(d.y, d.x);
    double beta = std::acos(2*r / D);
    t     = mod2pi(s.theta - phi + beta - M_PI/2);
    q     = mod2pi(e.theta - phi + beta - M_PI/2);
    p_out = std::sqrt(D2 - 4*r*r);
    return r*(t + q) + p_out;
}

static double dubinsRLR(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cR1 = circleCenter(s, r, false);
    Vec2 cR2 = circleCenter(e, r, false);
    Vec2 d   = cR2 - cR1;
    double D = d.norm();
    if (D > 4*r) return std::numeric_limits<double>::infinity();

    double phi   = std::atan2(d.y, d.x);
    double cosv  = D / (4*r);
    double v     = std::acos(std::max(-1.0, std::min(1.0, cosv)));
    t     = mod2pi(s.theta - phi - v + M_PI/2);
    p_out = mod2pi(M_PI - 2*v);
    q     = mod2pi(e.theta - phi + v + M_PI/2);
    return r*(t + p_out + q);
}

static double dubinsLRL(const Pose& s, const Pose& e, double r, double& t, double& p_out, double& q) {
    Vec2 cL1 = circleCenter(s, r, true);
    Vec2 cL2 = circleCenter(e, r, true);
    Vec2 d   = cL2 - cL1;
    double D = d.norm();
    if (D > 4*r) return std::numeric_limits<double>::infinity();

    double phi   = std::atan2(d.y, d.x);
    double cosv  = D / (4*r);
    double v     = std::acos(std::max(-1.0, std::min(1.0, cosv)));
    t     = mod2pi(phi - s.theta + v + M_PI/2);
    p_out = mod2pi(M_PI - 2*v);
    q     = mod2pi(phi - e.theta + v + M_PI/2);
    return r*(t + p_out + q);
}

// Calculeaza cel mai scurt Dubins path intre doua Pose-uri
DubinsPath shortestDubins(const Pose& start, const Pose& end, double r = MIN_TURN_RADIUS) {
    DubinsPath best;
    best.r     = r;
    best.start = start;
    best.end   = end;

    struct Candidate {
        DubinsType type;
        double (*fn)(const Pose&, const Pose&, double, double&, double&, double&);
    };

    static const Candidate candidates[] = {
        { DubinsType::LSL, dubinsLSL },
        { DubinsType::RSR, dubinsRSR },
        { DubinsType::LSR, dubinsLSR },
        { DubinsType::RSL, dubinsRSL },
        { DubinsType::RLR, dubinsRLR },
        { DubinsType::LRL, dubinsLRL },
    };

    for (auto& c : candidates) {
        double t, p, q;
        double len = c.fn(start, end, r, t, p, q);
        if (len < best.length) {
            best.type   = c.type;
            best.length = len;
            best.t      = t;
            best.p      = p;
            best.q      = q;
        }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────
//  WAYPOINT GENERATOR  –  pentru fiecare pilon
// ─────────────────────────────────────────────────────────────

/*
 * Genereaza un waypoint lateral fata de pilon.
 * Directia de abordare este calculata din pozitia waypoint-ului anterior.
 *
 * Regula WRO:
 *   ROSU  → rami pe DREAPTA → waypoint la DREAPTA pilonului
 *   VERDE → rami pe STANGA  → waypoint la STANGA  pilonului
 *
 * "Dreapta/Stanga" se refera la directia de mers in acel moment.
 */
Pose waypointForPillar(const Pillar& pillar,
                       const Vec2&   fromPos,
                       double        approachHeading,
                       double        offset = PASS_OFFSET)
{
    // Vectorul perpendicular pe directia de mers
    Vec2 fwd = { std::cos(approachHeading), std::sin(approachHeading) };
    Vec2 right = {  fwd.y, -fwd.x }; // perpendicular dreapta
    Vec2 left  = { -fwd.y,  fwd.x }; // perpendicular stanga

    Vec2 sideOffset = (pillar.color == PillarColor::RED) ? right : left;
    Vec2 wp = pillar.pos + sideOffset * offset;

    return Pose(wp.x, wp.y, approachHeading);
}

// ─────────────────────────────────────────────────────────────
//  CHECKER COLIZIUNE  –  verifica daca un Dubins path loveste piloni
// ─────────────────────────────────────────────────────────────

// Distanta de la un punct la un segment de dreapta
static double pointToSegmentDist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a, ap = p - a;
    double t = ap.dot(ab) / (ab.dot(ab) + 1e-12);
    t = std::max(0.0, std::min(1.0, t));
    Vec2 closest = a + ab * t;
    return (p - closest).norm();
}

// Distanta minima de la un punct la un arc de cerc
static double pointToArcDist(Vec2 p, Vec2 center, double r,
                               double startAngle, double sweepAngle)
{
    Vec2 dp = p - center;
    double ang = std::atan2(dp.y, dp.x);
    double s = mod2pi(startAngle);
    double sweep = sweepAngle; // pozitiv = CCW, negativ = CW

    // Normalizare sweep
    double relAng;
    if (sweep >= 0) relAng = mod2pi(ang - s);
    else            relAng = mod2pi(s - ang);

    double absSwep = std::abs(sweep);
    if (relAng <= absSwep) {
        // Proiectia e pe arc → distanta = |dist(p, center) - r|
        return std::abs(dp.norm() - r);
    } else {
        // Proiectia e in afara arcului → distanta la unul din capete
        double endAngle = s + sweep;
        Vec2 p1 = center + Vec2{std::cos(s),       std::sin(s)}       * r;
        Vec2 p2 = center + Vec2{std::cos(endAngle), std::sin(endAngle)} * r;
        return std::min((p - p1).norm(), (p - p2).norm());
    }
}

// Verifica daca un DubinsPath trece prea aproape de un pilon
bool pathClearOfPillar(const DubinsPath& path, const Pillar& pillar,
                        double safetyMargin = PILLAR_RADIUS + 30.0)
{
    const Pose& s   = path.start;
    const Pose& e   = path.end;
    double r        = path.r;

    // Decompune traseul in cele 3 segmente si verifica fiecare
    // Simplificat: esantionare densa de-a lungul traseului
    const int SAMPLES = 100;
    double totalLen = path.length;

    // Reconstruieste pozitiile de-a lungul traseului Dubins
    // (integrare numerica simpla)
    Pose cur = s;
    double dt = totalLen / SAMPLES;

    // Determinam tipurile de segmente
    std::string tstr = dubinsTypeName(path.type);

    for (int i = 0; i <= SAMPLES; ++i) {
        double t_param = (double)i / SAMPLES;
        double dist_along = t_param * totalLen;

        // Calculeaza pozitia aproximativa de-a lungul Dubins path
        // Segmentele: t (arc1), p (drept), q (arc2)
        double seg1_len = path.t * r;
        double seg2_len = path.p;
        double seg3_len = path.q * r;
        (void)seg3_len;

        Pose pos;
        if (dist_along < seg1_len) {
            double angle = dist_along / r;
            bool left1 = (tstr[0] == 'L');
            double turn = left1 ? angle : -angle;
            pos.x     = s.x + r * (std::sin(s.theta + turn) - std::sin(s.theta));
            pos.y     = s.y + r * (-std::cos(s.theta + turn) + std::cos(s.theta));
            pos.theta = s.theta + turn;
        } else if (dist_along < seg1_len + seg2_len) {
            double d = dist_along - seg1_len;
            double th1 = s.theta + (tstr[0]=='L' ? path.t : -path.t);
            pos.x = s.x + r*(std::sin(s.theta + (tstr[0]=='L'?path.t:-path.t)) - std::sin(s.theta))
                        + d * std::cos(th1);
            pos.y = s.y + r*(-std::cos(s.theta + (tstr[0]=='L'?path.t:-path.t)) + std::cos(s.theta))
                        + d * std::sin(th1);
            pos.theta = th1;
        } else {
            // Ultimul arc – aproximam ca pozitia finala
            pos = e;
        }

        double dist = (Vec2{pos.x, pos.y} - pillar.pos).norm();
        if (dist < safetyMargin) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
//  PLANIFICATOR PRINCIPAL
// ─────────────────────────────────────────────────────────────

struct WaypointResult {
    Pose        pose;
    int         pillarId;   // -1 daca e punct de colt/start/finish
    std::string label;
};

struct PlanResult {
    std::vector<WaypointResult> waypoints;
    std::vector<DubinsPath>     segments;
    double                      totalLength;
    bool                        valid;
};

/*
 * Planifica traseul complet pentru 3 tururi pe terenul WRO.
 *
 * Terenul e modelat ca un inel patrat; parcurgem sectiunile in ordine,
 * iar pentru fiecare pilon din sectiunea curenta generam un waypoint
 * lateral si il conectam cu Dubins path optim.
 */
PlanResult planPath(const std::vector<Pillar>& pillars,
                    bool clockwise = true,
                    int  laps      = 3)
{
    PlanResult result;
    result.totalLength = 0;
    result.valid       = true;

    // ── Waypoint-uri de colt ale terenului (sens orar: E→S→V→N) ──
    // Coordonate in mm, centrul terenului = (0,0)
    double half  = TRACK_OUTER / 2.0;
    double inner = TRACK_INNER / 2.0;

    // Punctele de mijloc ale celor 4 sectiuni drepte (unde robotul intra/iese)
    // Sens orar: Sus → Dreapta → Jos → Stanga
    struct Section {
        Pose entry;
        Pose exit;
    };

    // Definim sectiunile in sens orar
    // Sectiunea de start: Sus (Y = +half, X de la -inner la +inner)
    double midLane = inner + LANE_WIDTH / 2.0; // centrul benzii

    std::vector<Pose> cornerWaypoints;
    if (clockwise) {
        // Sens orar: start sus-stanga, mergi spre dreapta, cobori, stanga, urci
        cornerWaypoints = {
            Pose(-inner, +half - LANE_WIDTH/2, 0),            // intrare sus (heading Est)
            Pose(+half - LANE_WIDTH/2, +inner, -M_PI/2),      // intrare dreapta (heading Sud)
            Pose(+inner, -half + LANE_WIDTH/2, M_PI),          // intrare jos (heading Vest)
            Pose(-half + LANE_WIDTH/2, -inner, M_PI/2),        // intrare stanga (heading Nord)
        };
    } else {
        // Sens antiorar
        cornerWaypoints = {
            Pose(+inner, +half - LANE_WIDTH/2, M_PI),          // Sus heading Vest
            Pose(-half + LANE_WIDTH/2, +inner, -M_PI/2),       // Stanga heading Sud
            Pose(-inner, -half + LANE_WIDTH/2, 0),              // Jos heading Est
            Pose(+half - LANE_WIDTH/2, -inner, M_PI/2),        // Dreapta heading Nord
        };
    }

    // Sorteaza pilonii pe sectiuni (simplificat: dupa distanta fata de axa dominanta)
    // In practica, pilonii sunt deja in ordinea intalnita pe traseu

    // Construieste lista de waypoint-uri
    Pose currentPose = cornerWaypoints[0];
    result.waypoints.push_back({currentPose, -1, "START"});

    // Pentru fiecare tur
    for (int lap = 0; lap < laps; ++lap) {
        // Pentru fiecare sectiune din tur
        for (int sec = 0; sec < 4; ++sec) {
            int   sectionIdx    = (sec) % 4;
            Pose  sectionExit   = cornerWaypoints[(sectionIdx + 1) % 4];
            double sectionHeading = currentPose.theta;

            // Gaseste pilonii din aceasta sectiune
            // (simplificat: in ordinea in care apar pe directia de mers)
            std::vector<const Pillar*> sectionPillars;
            for (auto& p : pillars) {
                // Clasificam pilonul in sectiunea curenta dupa pozitie
                // (in implementare reala, se foloseste un indice de sectiune)
                double dx = p.pos.x - currentPose.x;
                double dy = p.pos.y - currentPose.y;
                double fwd = dx * std::cos(sectionHeading) + dy * std::sin(sectionHeading);
                if (fwd > 0 && fwd < TRACK_OUTER) {
                    sectionPillars.push_back(&p);
                }
            }

            // Sorteaza dupa distanta in directia de mers
            std::sort(sectionPillars.begin(), sectionPillars.end(),
                [&](const Pillar* a, const Pillar* b) {
                    double fa = (a->pos.x - currentPose.x) * std::cos(sectionHeading)
                              + (a->pos.y - currentPose.y) * std::sin(sectionHeading);
                    double fb = (b->pos.x - currentPose.x) * std::cos(sectionHeading)
                              + (b->pos.y - currentPose.y) * std::sin(sectionHeading);
                    return fa < fb;
                });

            // Genereaza waypoint pentru fiecare pilon
            for (auto* pl : sectionPillars) {
                Pose wp = waypointForPillar(*pl, currentPose.pos(), currentPose.theta);

                // Calculeaza Dubins path de la pozitia curenta la waypoint
                DubinsPath seg = shortestDubins(currentPose, wp);

                // Verifica coliziuni cu toti pilonii
                bool safe = true;
                for (auto& other : pillars) {
                    if (!pathClearOfPillar(seg, other)) {
                        safe = false;
                        break;
                    }
                }

                if (!safe) {
                    // Incearca cu offset mai mare
                    double bigOffset = PASS_OFFSET * 1.5;
                    Vec2 fwd = { std::cos(currentPose.theta), std::sin(currentPose.theta) };
                    Vec2 side = (pl->color == PillarColor::RED)
                                ? Vec2{ fwd.y, -fwd.x }
                                : Vec2{-fwd.y,  fwd.x };
                    Vec2 wpPos = pl->pos + side * bigOffset;
                    wp = Pose(wpPos.x, wpPos.y, currentPose.theta);
                    seg = shortestDubins(currentPose, wp);
                }

                std::string lbl = (pl->color == PillarColor::RED)
                    ? "Pilon ROSU #" + std::to_string(pl->id) + " → treci pe DREAPTA"
                    : "Pilon VERDE #" + std::to_string(pl->id) + " → treci pe STANGA";

                result.waypoints.push_back({wp, pl->id, lbl});
                result.segments.push_back(seg);
                result.totalLength += seg.length;
                currentPose = wp;
            }

            // Waypoint de colt (exit sectiune)
            DubinsPath cornerSeg = shortestDubins(currentPose, sectionExit);
            result.waypoints.push_back({sectionExit, -1,
                "Colt " + std::to_string(sectionIdx+1) + " (tur " + std::to_string(lap+1) + ")"});
            result.segments.push_back(cornerSeg);
            result.totalLength += cornerSeg.length;
            currentPose = sectionExit;
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────
//  AFISARE REZULTATE
// ─────────────────────────────────────────────────────────────

void printPath(const PlanResult& plan) {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cout <<   "║  WRO Future Engineers – Plan Traseu Optimal          ║\n";
    std::cout <<   "║  Dubins Car  |  Raza minima: " << std::setw(5) << MIN_TURN_RADIUS << " mm              ║\n";
    std::cout <<   "╚══════════════════════════════════════════════════════╝\n\n";

    std::cout << "WAYPOINT-URI  (" << plan.waypoints.size() << " total):\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(9)  << "X (mm)"
              << std::setw(9)  << "Y (mm)"
              << std::setw(9)  << "θ (°)"
              << std::setw(8)  << "Segment"
              << "Actiune\n";
    std::cout << std::string(70, '-') << "\n";

    for (size_t i = 0; i < plan.waypoints.size(); ++i) {
        const auto& wp  = plan.waypoints[i];
        double thetaDeg = wp.pose.theta * 180.0 / M_PI;

        std::cout << std::setw(4)  << i
                  << std::setw(9)  << wp.pose.x
                  << std::setw(9)  << wp.pose.y
                  << std::setw(9)  << thetaDeg;

        if (i > 0 && i-1 < plan.segments.size()) {
            const auto& seg = plan.segments[i-1];
            std::cout << std::setw(8) << dubinsTypeName(seg.type);
        } else {
            std::cout << std::setw(8) << "-";
        }
        std::cout << wp.label << "\n";
    }

    std::cout << std::string(70, '-') << "\n\n";

    std::cout << "SEGMENTE DUBINS:\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << std::setw(4)  << "#"
              << std::setw(7)  << "Tip"
              << std::setw(12) << "Arc1 (mm)"
              << std::setw(12) << "Drept (mm)"
              << std::setw(12) << "Arc2 (mm)"
              << std::setw(12) << "Total (mm)" << "\n";
    std::cout << std::string(70, '-') << "\n";

    for (size_t i = 0; i < plan.segments.size(); ++i) {
        const auto& s = plan.segments[i];
        std::cout << std::setw(4)  << i+1
                  << std::setw(7)  << dubinsTypeName(s.type)
                  << std::setw(12) << s.t * s.r
                  << std::setw(12) << s.p
                  << std::setw(12) << s.q * s.r
                  << std::setw(12) << s.length << "\n";
    }

    std::cout << std::string(70, '-') << "\n";
    std::cout << "\n✅ LUNGIME TOTALA TRASEU: "
              << plan.totalLength << " mm  ("
              << plan.totalLength / 1000.0 << " m)\n";

    if (!plan.valid)
        std::cout << "⚠️  ATENTIE: unele segmente pot intersecta piloni!\n";
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────
//  MAIN  –  exemplu cu piloni randomizati tipici WRO
// ─────────────────────────────────────────────────────────────

int main() {
    /*
     * Configuratie exemplu de piloni (adaptata din regulament WRO 2025).
     * Coordonate in mm fata de centrul terenului.
     * In competitie, acestia sunt plasati la intersectiile T/X din
     * sectiunile drepte.
     *
     * Sectiune SUS  (Y ≈ +400):
     *   - Pilon ROSU  la (-150, +380)   → treci pe dreapta
     *   - Pilon VERDE la (+100, +380)   → treci pe stanga
     * Sectiune DREAPTA (X ≈ +400):
     *   - Pilon VERDE la (+380, +100)   → treci pe stanga
     * Sectiune JOS (Y ≈ -400):
     *   - Pilon ROSU  la (+150, -380)   → treci pe dreapta
     *   - Pilon ROSU  la (-120, -380)   → treci pe dreapta
     * Sectiune STANGA (X ≈ -400):
     *   - Pilon VERDE la (-380, -80)    → treci pe stanga
     */

    std::vector<Pillar> pillars = {
        Pillar(-150, +380, PillarColor::RED,   1),
        Pillar(+100, +380, PillarColor::GREEN, 2),
        Pillar(+380, +100, PillarColor::GREEN, 3),
        Pillar(+150, -380, PillarColor::RED,   4),
        Pillar(-120, -380, PillarColor::RED,   5),
        Pillar(-380,  -80, PillarColor::GREEN, 6),
    };

    std::cout << "Configuratie teren:\n";
    std::cout << "  " << pillars.size() << " piloni | 3 tururi | sens: ORAR\n";
    std::cout << "  Raza minima viraj: " << MIN_TURN_RADIUS << " mm\n";
    std::cout << "  Offset trecere pilon: " << PASS_OFFSET << " mm\n\n";

    std::cout << "Piloni:\n";
    for (auto& p : pillars) {
        std::cout << "  #" << p.id
                  << "  " << (p.color == PillarColor::RED ? "ROSU " : "VERDE")
                  << "  (" << p.pos.x << ", " << p.pos.y << ")\n";
    }

    // Planifica traseul
    PlanResult plan = planPath(pillars, /*clockwise=*/true, /*laps=*/3);

    // Afiseaza rezultatele
    printPath(plan);

    // ── Test Dubins standalone ──
    std::cout << "═══ Test Dubins izolat ═══\n";
    Pose A(0, 0, 0);
    Pose B(500, 200, M_PI/4);
    DubinsPath dp = shortestDubins(A, B, MIN_TURN_RADIUS);
    std::cout << "De la (0,0,0°) la (500,200,45°):\n";
    std::cout << "  Tip:    " << dubinsTypeName(dp.type) << "\n";
    std::cout << "  Arc1:   " << std::fixed << std::setprecision(1) << dp.t * dp.r << " mm\n";
    std::cout << "  Drept:  " << dp.p  << " mm\n";
    std::cout << "  Arc2:   " << dp.q * dp.r << " mm\n";
    std::cout << "  TOTAL:  " << dp.length << " mm\n\n";

    return 0;
}
