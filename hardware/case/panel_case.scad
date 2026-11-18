// Remote control panel case — Waveshare ESP32-C6-Touch-LCD-1.47, KY-040
// rotary encoder, one 18650 cell. Parametric OpenSCAD; all sizes in mm.
//
//   part = "body"   front shell: screen window, knob hole, USB-C and switch cut-outs
//   part = "lid"    back cover with the 18650 cradle
//   part = "both"   both, side by side (preview)
//   part = "assembled"  body + lid in place, with ghost board/cell/encoder
//
// Render a part from the command line, e.g.:
//   openscad -D 'part="body"' -o panel_case_body.stl panel_case.scad
//
// Coordinates: X = width (left to right, looking at the screen), Y = height
// (bottom to top), Z = depth (0 = front face, growing towards the back).
//
// CHECK BEFORE PRINTING (see README.md): the KY-040 numbers, which vary
// between module makers — measure yours.

part = "both";

/* [Print] */
wall  = 2.0;   // side walls
front = 2.0;   // front plate (the encoder nut needs <= ~2.5)
lid_t = 2.0;   // back lid plate
tol   = 0.3;   // clearance on fitted parts
$fn   = 48;

/* [Display module — Waveshare drawing] */
mod_w  = 44.50;  // long side (landscape width)
mod_h  = 24.55;  // short side
mod_r  = 5.75;   // corner radius
mod_d  = 10.60;  // total thickness, glass to standoff tips
// Active area 32.93 x 17.75, 4.54 from the USB end and 7.03 from the other.
act_w  = 32.93;
act_h  = 17.75;
act_usb_gap   = 4.54;
win_margin    = 1.0;   // window larger than the active area by this per side
// USB-C sits on the module's short edge, centred; its centre is about this
// far behind the glass (PCB back + connector half-height).
usb_z  = 8.2;
usb_cut_w = 13.0;  // fits common USB-C plug overmoulds
usb_cut_h = 7.5;
// Which side the USB-C port is on when the UI reads upright (confirmed on
// the board: right, with the firmware's landscape rotation). The knob goes on
// the opposite side.
usb_side = "right";  // "left" | "right"

/* [KY-040 encoder module] */
ky_w   = 19.0;   // PCB width
ky_l   = 32.0;   // PCB length (pins end at the bottom)
ky_shaft_from_top = 12.5;  // shaft centre from the PCB's top edge
ky_bush_d   = 7.0;   // threaded bushing (M7)
ky_pin_room = 7.0;   // below the PCB: header pins, soldered wires
ky_depth    = 12.0;  // PCB back + parts, measured from the front plate's inside

/* [Battery — one 18650] */
batt_d   = 18.6;
batt_len = 70.0;   // protected cells are ~67-70; bare ~65
batt_gap = 2.0;    // around the cell

/* [Power switch — SS12D00-style mini slide switch] */
switch_on    = true;
switch_slot  = [4.5, 2.6];   // actuator slot in the bottom wall (x, z)

/* [Fasteners] */
screw_pilot = 2.5;   // M3 self-tapping into the corner posts
screw_hole  = 3.4;   // clearance in the lid
screw_head  = 6.0;   // countersink diameter
post_d      = 7.0;

// ---------------------------------------------------------------------------
// Derived layout

gap_usb   = 1.0;   // module to the USB-side wall: keeps the plug reachable
gap_mid   = 5.0;   // module to encoder PCB
gap_knob  = 1.5;   // encoder PCB to its wall

// The cell runs across the middle band; the corner posts sit above and below
// it, so only the cell and its end stops set the minimum width.
in_w = max(gap_usb + mod_w + gap_mid + ky_w + gap_knob, batt_len + 2 * (1.5 + 2) + 2 * batt_gap);
in_h = max(ky_l + ky_pin_room + 2, mod_h + 2 * 4);
zone1 = max(mod_d, ky_depth) + 1.5;          // board / encoder layer
body_d = front + zone1 + batt_d + batt_gap;  // front face to lid seat

out_w = in_w + 2 * wall;
out_h = in_h + 2 * wall;

// Positions in the inside frame (x 0..in_w, y 0..in_h), for usb_side = "left";
// mirrored for "right".
function mx(x) = usb_side == "left" ? x : in_w - x;

mod_x0  = gap_usb;                       // module's USB end
mod_cy  = in_h / 2;
knob_x  = in_w - gap_knob - ky_w / 2;    // knob on the far side from USB
knob_y  = in_h - 1 - ky_shaft_from_top;  // PCB top edge 1 mm below the ceiling
win_w   = act_w + 2 * win_margin;
win_h   = act_h + 2 * win_margin;
win_cx  = mod_x0 + act_usb_gap + act_w / 2;  // active area sits nearer the USB end

module rrect(w, h, r) {
  offset(r) offset(-r) square([w, h], center = true);
}

// ---------------------------------------------------------------------------
// Body

module body() {
  difference() {
    union() {
      // Shell: front plate + walls, open at the back.
      difference() {
        translate([-wall, -wall, 0]) linear_extrude(body_d) offset(2) offset(-2)
          square([out_w, out_h]);
        translate([0, 0, front]) linear_extrude(body_d) square([in_w, in_h]);
      }
      // Locating rim around the display module (press fit; a few dots of hot
      // glue or 1 mm foam tape on the rim hold it for good).
      translate([mx(mod_x0 + mod_w / 2), mod_cy, front]) linear_extrude(3)
        difference() {
          rrect(mod_w + 2 * tol + 2.4, mod_h + 2 * tol + 2.4, mod_r + 1.2);
          rrect(mod_w + 2 * tol, mod_h + 2 * tol, mod_r);
          // Gap in the rim at the USB end so the connector area stays clear.
          translate([(usb_side == "left" ? -1 : 1) * mod_w / 2, 0])
            square([6, usb_cut_w], center = true);
        }
      // Corner posts for the lid screws.
      for (p = posts()) translate([p[0], p[1], front])
        cylinder(d = post_d, h = body_d - front);
    }
    // Screen window, chamfered towards the front so a finger reaches the edges.
    translate([mx(win_cx), mod_cy, -0.01])
      hull() {
        linear_extrude(0.01) rrect(win_w + 2 * front, win_h + 2 * front, 3);
        translate([0, 0, front]) linear_extrude(0.02) rrect(win_w, win_h, 1.5);
      }
    // Encoder bushing hole.
    translate([mx(knob_x), knob_y, -1]) cylinder(d = ky_bush_d + 0.5, h = front + 2);
    // USB-C opening in the USB-side wall.
    translate([usb_side == "left" ? -wall - 1 : in_w - 1, mod_cy, front + usb_z])
      rotate([0, 90, 0]) linear_extrude(wall + 2) rrect(usb_cut_h, usb_cut_w, 2);
    // Slide switch actuator slot in the bottom wall, in the battery zone.
    if (switch_on)
      translate([mx(in_w * 0.35), -wall / 2, front + zone1 + batt_d / 2])
        cube([switch_slot[0], wall + 2, switch_slot[1]], center = true);
    // Lid screw pilots.
    for (p = posts()) translate([p[0], p[1], body_d - 12])
      cylinder(d = screw_pilot, h = 13);
  }
}

function posts() = [
  [post_d / 2, post_d / 2], [in_w - post_d / 2, post_d / 2],
  [post_d / 2, in_h - post_d / 2], [in_w - post_d / 2, in_h - post_d / 2]];

// ---------------------------------------------------------------------------
// Lid: back plate with an inset lip and the 18650 cradle on its inside.
// Modelled in place (z from body_d), then laid flat for printing.

batt_cz = body_d - batt_gap / 2 - batt_d / 2;   // cell axis depth

module lid_in_place() {
  difference() {
    union() {
      // Plate
      translate([-wall, -wall, body_d]) linear_extrude(lid_t) offset(2) offset(-2)
        square([out_w, out_h]);
      // Lip that drops into the opening (clears the posts).
      translate([0, 0, body_d - 1.5]) linear_extrude(1.5) difference() {
        offset(-tol) square([in_w, in_h]);
        offset(-tol - 1.6) square([in_w, in_h]);
        for (p = posts()) translate(p) circle(d = post_d + 2 * tol);
      }
      // Cradle: two saddles under the cell.
      for (sx = [in_w / 2 - batt_len * 0.3, in_w / 2 + batt_len * 0.3])
        translate([sx, mod_cy, 0]) cradle_saddle();
      // End stops.
      for (ex = [in_w / 2 - batt_len / 2 - 1.5, in_w / 2 + batt_len / 2 + 1.5])
        translate([ex - 1, mod_cy - 6, batt_cz]) cube([2, 12, body_d - batt_cz]);
    }
    // Screw holes, countersunk from the outside.
    for (p = posts()) translate([p[0], p[1], body_d - 2]) {
      cylinder(d = screw_hole, h = lid_t + 4);
      translate([0, 0, 2 + lid_t - 1.6]) cylinder(d1 = screw_hole, d2 = screw_head, h = 1.61);
    }
    // The cell itself.
    translate([in_w / 2 - batt_len / 2, mod_cy, batt_cz]) rotate([0, 90, 0])
      cylinder(d = batt_d + tol, h = batt_len);
  }
}

module cradle_saddle() {
  difference() {
    translate([-4, -batt_d / 2 - 2, batt_cz]) cube([8, batt_d + 4, body_d - batt_cz]);
    // Zip-tie slot through the saddle (3.5 x 1.6 ties).
    translate([-1.75, -batt_d / 2 - 3, body_d - 2.2]) cube([3.5, batt_d + 6, 1.8]);
  }
}

module lid() {
  // Flip to print plate-down.
  translate([0, 0, body_d + lid_t]) mirror([0, 0, 1]) lid_in_place();
}

// ---------------------------------------------------------------------------
// Ghost parts for the assembled preview.

module ghosts() {
  %translate([mx(mod_x0 + mod_w / 2), mod_cy, front]) linear_extrude(mod_d)
    rrect(mod_w, mod_h, mod_r);
  %translate([mx(knob_x) - ky_w / 2, knob_y + ky_shaft_from_top - ky_l, front + 7])
    cube([ky_w, ky_l, 1.6]);
  %translate([mx(knob_x), knob_y, -15]) cylinder(d = 6, h = 22);
  %translate([in_w / 2 - batt_len / 2, mod_cy, batt_cz]) rotate([0, 90, 0])
    cylinder(d = batt_d, h = batt_len);
}

// ---------------------------------------------------------------------------

echo(str("Case outside: ", out_w, " x ", out_h, " x ", body_d + lid_t, " mm"));

if (part == "body") {
  body();
} else if (part == "lid") {
  lid();
} else if (part == "both") {
  body();
  translate([out_w + 10, 0, 0]) lid();
} else if (part == "assembled") {
  body();
  lid_in_place();
  ghosts();
}
