// =====================================================================
//  BLEhound case · Variant A (tray + lid)
//  - 4x M3 countersunk from the bottom, through the tray bosses and the PCB mounting holes, into heat-set inserts in the lid bosses
//  - Top face has only the 4 LED holes (+ engraved text); buttons/SWD require opening the lid
//  - Antenna-end SMA holes sit on the parting line (parting line = SMA axis 6.0), half-circle in tray and half in lid; USB-C exits the rear wall via a 9.8x4.0 cutout
//  Coordinate frame: PCB bottom-left = (0,0), PCB top face = z 0 (matches the delivered coordinate file, x = MidX-113.5, y = MidY+135)
//  Render: openscad -o tray.stl -D 'part="tray"' blehound_case_A.scad
//        openscad -o lid.stl  -D 'part="lid"'  blehound_case_A.scad   (lid is pre-flipped, print as-is)
// =====================================================================

part = "both";        // "tray" | "lid" | "both" (both = assembly preview, includes PCB)
explode = 0;          // explode distance for the assembly preview (mm)

// ---------- PCB (from the delivered file, do not edit) ----------
pcb_w = 70;  pcb_d = 60;  pcb_t = 1.6;
holes  = [[4,4],[66,4],[4,47.095],[66,47.095]];        // M3 Ø3.2
sma_x  = [17.6, 40.8, 64.0];                            // J1 J4 J7 (populated)
ant2_x = [6.0, 29.2, 52.4];                             // J2 J5 J8 (reserved, no slot by default)
leds   = [[13.833,23.26],[34.436,24.022],[56.055,23.26],[44.883,7.202]];   // D1 D2 D3 D4
led_names = ["CH37","CH38","CH39","PWR"];
sw_pos = [[6.017,18.915],[15.44,18.915],[24.863,18.915],[45.336,18.915],[55.059,18.915],[64.782,18.915]];
hdr_x  = [23.933, 45.408, 66.429];  hdr_y = 35.329;     // 2x5 pin header
usb_x  = 35;

// ---------- Connector measurements/specs ----------
// SMA-KWE external-thread inner-hole right-angle jack (overall 14.5, thread 8.5, body 6x6x9.2, pin 3.5, pin pitch 5.1x5.1)
// Body front face is 0.4 beyond the front pin row -> front face 1.0 inside the board edge; thread extends 8.5 from the face -> 7.5 beyond the board edge
sma_axis_z   = 6.0;    // thread axis above the PCB top face: body height 9.2, thread Ø6.35 top flush with body top -> 9.2-3.2
sma_body_h   = 9.2;
sma_body_y0  = 53.0;   sma_body_y1 = 59.0;
sma_thread_l = 8.5;
sma_slot_d   = 7.4;    // thread Ø6.35 + axis-height ±0.3 margin; parting line runs through the axis, half-circle top and bottom
usb_protrude = 1.4;    // USB-C receptacle overhang past the board edge (measured)
usb_cut_w    = 9.8;    // receptacle body 8.94x3.26 + 0.4 per side; plug metal shell 8.3x2.6 fits through, overmold stays outside the wall
usb_cut_h    = 4.0;
usb_cut_r    = 1.3;
usb_chamfer  = 0.8;    // 45° lead-in chamfer on the outer side of the cutout
ant2_slots   = false;  // set true once ANT2 is populated (adds 3 more half-circle slots in the front wall, lid follows)

// ---------- Case parameters ----------
clr        = 0.5;      // clearance around the PCB
wall       = 2.0;
floor_t    = 2.0;
lid_t      = 2.0;
standoff_h = 4.0;      // PCB bottom to tray inner floor: through-hole pin tails + solder joints
inner_h    = 10.0;     // PCB top to lid inner ceiling: pin header 8.5 + 1.5
part_z     = sma_axis_z;   // parting line through the SMA axis; SMA hole is half-circle top/bottom; board drops in flat
corner_r   = 3.0;      // outer corner radius
boss_d     = 8.0;      // boss diameter, top and bottom (M3 heat-set insert OD 4.6, wall ≈1.7)
screw_d    = 3.4;      // M3 clearance hole
csk_d      = 6.4;      // countersink top diameter (M3 flat head 90°)
csk_depth  = 1.5;
insert_d   = 4.0;      // M3x4x5 heat-set insert pilot hole (some batches need 4.2)
insert_h   = 6.5;      // insert 5 + screw-tip margin
lip_h      = 1.5;      // depth the lid inner lip reaches into the tray
lip_t      = 1.2;
lip_clr    = 0.2;
led_hole_d = 2.2;
led_tube_d = 4.6;      // light-pipe OD under the lid
led_tube_bottom_z = 2.5;   // light-pipe lower end above the PCB top face (LED height 0.7, button height 1.5)
pin_holes  = false;    // when true, add a Ø2.2 pinhole in the top face over each of the 6 buttons
pin_hole_d = 2.2;
feet_pos   = [[12,10],[58,10],[12,41],[58,41]];   // foot-pad positions (clear of the screws)
foot_d     = 8;  foot_depth = 1.0;
text_top   = "BLEhound";
text_depth = 0.4;
$fn = 64;

// ---------- Derived ----------
ox0 = -clr - wall;   ox1 = pcb_w + clr + wall;
oy0 = -clr - wall;   oy1 = pcb_d + clr + wall;
z_floor = -pcb_t - standoff_h - floor_t;     // -7.6
z_top   = inner_h + lid_t;                   // 12.0
total_h = z_top - z_floor;                   // 19.6
echo(str("Outer ", ox1-ox0, " x ", oy1-oy0, " x ", total_h, " mm; suggested M3 screw length 12 (into insert ", 12-(floor_t+standoff_h+pcb_t), " mm)"));
echo(str("SMA thread past the wall ", sma_thread_l - (pcb_d - sma_body_y1) - clr - wall, " mm (after a 2.2 nut, ", sma_thread_l - (pcb_d - sma_body_y1) - clr - wall - 2.2, " left for the antenna)"));

module rrect(x0,y0,x1,y1,r,h){          // rounded rectangular prism, z from 0 to h
  hull() for (x=[x0+r,x1-r], y=[y0+r,y1-r]) translate([x,y,0]) cylinder(r=r,h=h);
}
module outer_block(z0,z1){ translate([0,0,z0]) rrect(ox0,oy0,ox1,oy1,corner_r,z1-z0); }
module cavity_block(z0,z1){ translate([0,0,z0]) rrect(-clr,-clr,pcb_w+clr,pcb_d+clr,max(corner_r-wall,0.6),z1-z0); }

// ================= Tray =================
module tray(){
  difference(){
    union(){
      difference(){
        outer_block(z_floor, part_z);
        cavity_block(z_floor+floor_t, part_z+1);
      }
      // bosses: inner floor -> PCB bottom
      for (h=holes) translate([h[0],h[1],z_floor+floor_t-0.01]) cylinder(d=boss_d, h=standoff_h+0.01);
    }
    // M3 clearance hole + bottom countersink
    for (h=holes) translate([h[0],h[1],0]) {
      translate([0,0,z_floor-1]) cylinder(d=screw_d, h=floor_t+standoff_h+2);
      translate([0,0,z_floor-0.01]) cylinder(d1=csk_d, d2=screw_d, h=csk_depth);
    }
    // front-wall SMA holes: full circle across the parting line; tray gets the lower half (opening up)
    sma_holes();
    // rear-wall USB-C cutout (closed, rounded, outer lead-in chamfer); receptacle center height 1.63
    usb_cut();
    // foot-pad recesses
    for (f=feet_pos) translate([f[0],f[1],z_floor-0.01]) cylinder(d=foot_d, h=foot_depth+0.01);
  }
}

module sma_holes(){
  for (x = concat(sma_x, ant2_slots ? ant2_x : []))
    translate([x, pcb_d+clr-1, sma_axis_z]) rotate([-90,0,0]) cylinder(d=sma_slot_d, h=wall+2);
}
module usb_cut(){
  zc = 3.26/2;
  translate([usb_x, -clr-wall-1, zc]) rotate([-90,0,0]) {
    linear_extrude(wall+2) offset(r=usb_cut_r) offset(delta=-usb_cut_r) square([usb_cut_w, usb_cut_h], center=true);
    // outer chamfer: usb_chamfer deep from the outer surface, opening larger by 2*usb_chamfer
    hull(){
      translate([0,0,1-0.01]) linear_extrude(0.01) offset(r=usb_cut_r+usb_chamfer) offset(delta=-usb_cut_r) square([usb_cut_w, usb_cut_h], center=true);
      translate([0,0,1+usb_chamfer]) linear_extrude(0.01) offset(r=usb_cut_r) offset(delta=-usb_cut_r) square([usb_cut_w, usb_cut_h], center=true);
    }
  }
}

// ================= Lid (assembly orientation) =================
module lid(){
  difference(){
    union(){
      difference(){
        outer_block(part_z, z_top);
        cavity_block(part_z-1, inner_h);
      }
      // inner lip (reaches into the tray, locating); cleared at the SMA positions
      // two parts: a bonding ring inside the skirt (overlaps the skirt inner wall to stay one solid) + a locating ring below the parting line, smaller than the tray inner wall by lip_clr
      difference(){
        union(){
          translate([0,0,part_z-0.01]) rrect(-clr,-clr,pcb_w+clr,pcb_d+clr,max(corner_r-wall,0.6),1.0+0.01);
          translate([0,0,part_z-lip_h]) rrect(-clr+lip_clr,-clr+lip_clr,pcb_w+clr-lip_clr,pcb_d+clr-lip_clr,max(corner_r-wall-lip_clr,0.5),lip_h+0.02);
        }
        translate([0,0,part_z-lip_h-1]) rrect(-clr+lip_clr+lip_t,-clr+lip_clr+lip_t,pcb_w+clr-lip_clr-lip_t,pcb_d+clr-lip_clr-lip_t,0.5,lip_h+4);
        for (x = concat(sma_x, ant2_slots ? ant2_x : [])) translate([x-sma_slot_d/2-1, pcb_d-3, part_z-lip_h-1]) cube([sma_slot_d+2, 8, lip_h+4]);
      }
      // bosses: PCB top -> inner ceiling
      for (h=holes) translate([h[0],h[1],0]) cylinder(d=boss_d, h=inner_h+0.01);
      // LED light pipes
      for (l=leds) translate([l[0],l[1],led_tube_bottom_z]) cylinder(d=led_tube_d, h=inner_h-led_tube_bottom_z+0.01);
    }
    // SMA hole upper half (skirt)
    sma_holes();
    // heat-set insert holes (from the boss lower end)
    for (h=holes) translate([h[0],h[1],-0.01]) cylinder(d=insert_d, h=insert_h);
    // LED holes
    for (l=leds) translate([l[0],l[1],led_tube_bottom_z-1]) cylinder(d=led_hole_d, h=inner_h+lid_t+2);
    // optional button pinholes
    if (pin_holes) for (p=sw_pos) translate([p[0],p[1],inner_h-1]) cylinder(d=pin_hole_d, h=lid_t+2);
    // engraved text on the top face
    translate([0,0,z_top-text_depth]) linear_extrude(text_depth+0.01) {
      translate([pcb_w/2, 50]) text(text_top, size=5, halign="center", valign="center", font="Liberation Sans:style=Bold");
      for (i=[0:3]) translate([leds[i][0], leds[i][1]-3.6]) text(led_names[i], size=2.2, halign="center", valign="center", font="Liberation Sans");
    }
  }
}

// ================= PCB placeholder (preview only) =================
module pcb_preview(){
  color("darkgreen") translate([0,0,-pcb_t]) cube([pcb_w,pcb_d,pcb_t]);
  color("black") for (x=hdr_x) translate([x-2.54,hdr_y-6.35,0]) cube([5.08,12.7,2.5]);
  color("gold") for (x=hdr_x, r=[0,1], c=[0:4]) translate([x-1.27+r*2.54-0.32, hdr_y-5.08+c*2.54-0.32, 0]) cube([0.64,0.64,8.5]);
  color("silver") translate([usb_x-4.47,-usb_protrude,0]) cube([8.94,7.35,3.26]);
  for (x=sma_x) {
    color("silver") translate([x-3,sma_body_y0,0]) cube([6,sma_body_y1-sma_body_y0,sma_body_h]);
    color("gold") translate([x,sma_body_y1,sma_axis_z]) rotate([-90,0,0]) cylinder(d=6.35,h=sma_thread_l);
    color("gold") translate([x,pcb_d+clr+wall+0.3,sma_axis_z]) rotate([-90,0,0]) cylinder(d=8/cos(30),h=2.2,$fn=6);   // nut outside the front wall (optional)
  }
  color("lightgreen") for (l=leds) translate([l[0]-0.8,l[1]-0.4,0]) cube([1.6,0.8,0.7]);
  color("dimgray") for (p=sw_pos) translate([p[0]-2.6,p[1]-2.6,0]) cube([5.2,5.2,1.5]);
}

// ================= Output =================
if (part == "tray") tray();
else if (part == "lid") {
  // flip: top face down on the bed
  translate([0,oy0+oy1,z_top]) rotate([180,0,0]) lid();   // still lands within the original XY range after flipping
}
else {
  color("SteelBlue", 0.85) translate([0,0,-explode]) tray();
  pcb_preview();
  color("LightSteelBlue", 0.7) translate([0,0,explode]) lid();
  // preview screws
  color("gray") for (h=holes) translate([h[0],h[1],z_floor-2*explode]) cylinder(d=3,h=12);
}
