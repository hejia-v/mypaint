/* This file is part of MyPaint.
 * Copyright (C) 2007 by Martin Renold <martinxyz@gmx.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY. See the COPYING file for more details.
 */

//#include "flup.hpp"

extern "C" {
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <math.h>
#include "Python.h"

#include "brushsettings.h"
#include "mapping.h"
}

#define ACTUAL_RADIUS_MIN 0.2
#define ACTUAL_RADIUS_MAX 150 //FIXME: performance problem actually depending on CPU

/* The Brush class stores two things:
   a) the states of the cursor (velocity, color, speed)
   b) the brush settings (as set in the GUI)
   FIXME: Actually those are two orthogonal things. Should separate them.
          (There used to be code that could change the settings during a
           stroke, this is hopefully already all gone.)
          (There might still be some "not-sure-if-those-are-states" though.)

   In python, there are two kinds of instances from this: a "global
   brush" which does the cursor tracking, and the "brushlist" where
   only the settings are important. When a brush is selected, its
   settings are copied into the global one, leaving the status intact.
   FIXME: if only the settings are important there is no need a full instance of this class
          it is rather the other way around: in theory this brush class should be a
          subclass of the class storing only the settings?
 */


/*
// prototypes
void settings_base_values_have_changed ();
void split_stroke ();
*/

class Brush {
private:
  GRand * rng;

  // see also brushsettings.py

  // those are not brush states, just convenience instead of function arguments
  // FIXME: if the above comment is really correct (which I doubt) then those should be moved into RenderContext
  float dx, dy, dpressure, dtime; // note: this is dx/ddab, ..., dtime/ddab (dab number, 5.0 = 5th dab)

  // the current value of a setting
  // FIXME: they could as well be passed as parameters to the dab function
  //        (Hm. This way no malloc is needed before each dab. Think about that.)
  float settings_value[BRUSH_SETTINGS_COUNT];

  // the mappings that describe how to calculate the current value for each setting
  Mapping * settings[BRUSH_SETTINGS_COUNT];

public:
  bool print_inputs; // debug menu
  Rect stroke_bbox; // track it here, get/reset from python
  double stroke_total_painting_time;
  double stroke_idling_time; 

private:
  // the states (get_state, set_state, reset) that change during a stroke
  float states[STATE_COUNT];
  bool exception; // set if a python exception is pending (thrown in a callback)

  // cached calculation results
  float speed_mapping_gamma[2], speed_mapping_m[2], speed_mapping_q[2];

  PyObject * split_stroke_callback;

public:
  Brush() {
    int i;
    for (i=0; i<BRUSH_SETTINGS_COUNT; i++) {
      settings[i] = mapping_new(INPUT_COUNT);
    }
    rng = g_rand_new();
    print_inputs = false;
    
    exception = false;
    split_stroke_callback = NULL;
    // initializes remaining members
    split_stroke();

    // FIXME: nobody can attach a listener to get events fired from the constructor
    settings_base_values_have_changed();

  }

  ~Brush() {
    int i;
    for (i=0; i<BRUSH_SETTINGS_COUNT; i++) {
      mapping_free(settings[i]);
    }
    g_rand_free (rng); rng = NULL;

    Py_CLEAR (split_stroke_callback);
  }

  void set_base_value (int id, float value) {
    g_assert (id >= 0 && id < BRUSH_SETTINGS_COUNT);
    Mapping * m = settings[id];
    m->base_value = value;

    settings_base_values_have_changed ();
  }

  void set_mapping_n (int id, int input, int n) {
    g_assert (id >= 0 && id < BRUSH_SETTINGS_COUNT);
    Mapping * m = settings[id];
    mapping_set_n (m, input, n);
  }

  void set_mapping_point (int id, int input, int index, float x, float y) {
    g_assert (id >= 0 && id < BRUSH_SETTINGS_COUNT);
    Mapping * m = settings[id];
    mapping_set_point (m, input, index, x, y);
  }

  // returns the fraction still left after t seconds (TODO: maybe move this out of this class)
  float exp_decay (float T_const, float t)
  {
    // the argument might not make mathematical sense (whatever.)
    if (T_const <= 0.001) {
      return 0.0;
    } else {
      return exp(- t / T_const);
    }
  }


  void settings_base_values_have_changed ()
  {
    // precalculate stuff that does not change dynamically

    // Precalculate how the physical speed will be mapped to the speed input value.
    // The forumla for this mapping is:
    //
    // y = log(gamma+x)*m + q;
    //
    // x: the physical speed (pixels per basic dab radius)
    // y: the speed input that will be reported
    // gamma: parameter set by ths user (small means a logarithmic mapping, big linear)
    // m, q: parameters to scale and translate the curve
    //
    // The code below calculates m and q given gamma and two hardcoded constraints.
    //
    int i;
    for (i=0; i<2; i++) {
      float gamma;
      gamma = settings[(i==0)?BRUSH_SPEED1_GAMMA:BRUSH_SPEED2_GAMMA]->base_value;
      gamma = exp(gamma);

      float fix1_x, fix1_y, fix2_x, fix2_dy;
      fix1_x = 45.0;
      fix1_y = 0.5;
      fix2_x = 45.0;
      fix2_dy = 0.015;
      //fix1_x = 45.0;
      //fix1_y = 0.0;
      //fix2_x = 45.0;
      //fix2_dy = 0.015;

      float m, q;
      float c1;
      c1 = log(fix1_x+gamma);
      m = fix2_dy * (fix2_x + gamma);
      q = fix1_y - m*c1;
    
      //g_print("a=%f, m=%f, q=%f    c1=%f\n", a, m, q, c1);

      speed_mapping_gamma[i] = gamma;
      speed_mapping_m[i] = m;
      speed_mapping_q[i] = q;
    }
  }

  // Update the "important" settings. (eg. actual radius, velocity)
  // FIXME: states, not settings! rename!
  //
  // This has to be done more often than each dab, because of
  // interpolation. For example if the radius is very big and suddenly
  // changes to very small, then lots of time might pass until a dab
  // would happen. But with the updated smaller radius, much more dabs
  // should have been painted already.

  void brush_update_settings_values ()
  {
    int i;
    float pressure;
    float inputs[INPUT_COUNT];

    if (dtime < 0.0) {
      printf("Time is running backwards!\n");
      dtime = 0.00001;
    } else if (dtime == 0.0) {
      // FIXME: happens about every 10th start, workaround (against division by zero)
      dtime = 0.00001;
    }

    float base_radius = expf(settings[BRUSH_RADIUS_LOGARITHMIC]->base_value);

    // FIXME: does happen (interpolation problem?)
    if (states[STATE_PRESSURE] < 0.0) states[STATE_PRESSURE] = 0.0;
    if (states[STATE_PRESSURE] > 1.0) states[STATE_PRESSURE] = 1.0;
    g_assert (states[STATE_PRESSURE] >= 0.0 && states[STATE_PRESSURE] <= 1.0);
    pressure = states[STATE_PRESSURE]; // could distort it here

    { // start / end stroke (for "stroke" input only)
      if (!states[STATE_STROKE_STARTED]) {
        if (pressure > settings[BRUSH_STROKE_TRESHOLD]->base_value + 0.0001) {
          // start new stroke
          //printf("stroke start %f\n", pressure);
          states[STATE_STROKE_STARTED] = 1;
          states[STATE_STROKE] = 0.0;
        }
      } else {
        if (pressure <= settings[BRUSH_STROKE_TRESHOLD]->base_value * 0.9 + 0.0001) {
          // end stroke
          //printf("stroke end\n");
          states[STATE_STROKE_STARTED] = 0;
        }
      }
    }

    // now follows input handling

    float norm_dx, norm_dy, norm_dist, norm_speed;
    norm_dx = dx / dtime / base_radius;
    norm_dy = dy / dtime / base_radius;
    norm_speed = sqrt(SQR(norm_dx) + SQR(norm_dy));
    norm_dist = norm_speed * dtime;

    inputs[INPUT_PRESSURE] = pressure;
    inputs[INPUT_SPEED1] = log(speed_mapping_gamma[0] + states[STATE_NORM_SPEED1_SLOW])*speed_mapping_m[0] + speed_mapping_q[0];
    inputs[INPUT_SPEED2] = log(speed_mapping_gamma[1] + states[STATE_NORM_SPEED2_SLOW])*speed_mapping_m[1] + speed_mapping_q[1];
    inputs[INPUT_RANDOM] = g_rand_double (rng);
    inputs[INPUT_STROKE] = MIN(states[STATE_STROKE], 1.0);
    //if (states[STATE_NORM_DY_SLOW] == 0 && states[STATE_NORM_DX_SLOW] == 0) {
    inputs[INPUT_ANGLE] = fmodf(atan2f (states[STATE_NORM_DY_SLOW], states[STATE_NORM_DX_SLOW])/(M_PI) + 1.0, 1.0);
    inputs[INPUT_CUSTOM] = states[STATE_CUSTOM_INPUT];
    if (print_inputs) {
      g_print("press=% 4.3f, speed1=% 4.4f\tspeed2=% 4.4f\tstroke=% 4.3f\tcustom=% 4.3f\n", (double)inputs[INPUT_PRESSURE], (double)inputs[INPUT_SPEED1], (double)inputs[INPUT_SPEED2], (double)inputs[INPUT_STROKE], (double)inputs[INPUT_CUSTOM]);
    }
    assert(inputs[INPUT_SPEED1] >= 0.0 && inputs[INPUT_SPEED1] < 1e8); // checking for inf

    // OPTIMIZE:
    // Could only update those settings that can influence the dabbing process here.
    // (the ones only relevant for the actual drawing could be updated later)
    // However, this includes about half of the settings already. So never mind.
    for (i=0; i<BRUSH_SETTINGS_COUNT; i++) {
      settings_value[i] = mapping_calculate (settings[i], inputs);
    }

    {
      float fac = 1.0 - exp_decay (settings_value[BRUSH_SLOW_TRACKING_PER_DAB], 1.0);
      states[STATE_ACTUAL_X] += (states[STATE_X] - states[STATE_ACTUAL_X]) * fac; // FIXME: should this depend on base radius?
      states[STATE_ACTUAL_Y] += (states[STATE_Y] - states[STATE_ACTUAL_Y]) * fac;
    }

    { // slow speed
      float fac;
      fac = 1.0 - exp_decay (settings_value[BRUSH_SPEED1_SLOWNESS], dtime);
      states[STATE_NORM_SPEED1_SLOW] += (norm_speed - states[STATE_NORM_SPEED1_SLOW]) * fac;
      fac = 1.0 - exp_decay (settings_value[BRUSH_SPEED2_SLOWNESS], dtime);
      states[STATE_NORM_SPEED2_SLOW] += (norm_speed - states[STATE_NORM_SPEED2_SLOW]) * fac;
    }
  
    { // slow speed, but as vector this time
      float fac = 1.0 - exp_decay (exp(settings_value[BRUSH_OFFSET_BY_SPEED_SLOWNESS]*0.01)-1.0, dtime);
      states[STATE_NORM_DX_SLOW] += (norm_dx - states[STATE_NORM_DX_SLOW]) * fac;
      states[STATE_NORM_DY_SLOW] += (norm_dy - states[STATE_NORM_DY_SLOW]) * fac;
    }

    { // custom input
      float fac;
      fac = 1.0 - exp_decay (settings_value[BRUSH_CUSTOM_INPUT_SLOWNESS], 0.1);
      states[STATE_CUSTOM_INPUT] += (settings_value[BRUSH_CUSTOM_INPUT] - states[STATE_CUSTOM_INPUT]) * fac;
    }

    { // stroke length
      float frequency;
      float wrap;
      frequency = expf(-settings_value[BRUSH_STROKE_DURATION_LOGARITHMIC]);
      states[STATE_STROKE] += norm_dist * frequency;
      //FIXME: why can this happen?
      if (states[STATE_STROKE] < 0) states[STATE_STROKE] = 0;
      //assert(stroke >= 0);
      wrap = 1.0 + settings_value[BRUSH_STROKE_HOLDTIME];
      if (states[STATE_STROKE] > wrap) {
        if (wrap > 9.9 + 1.0) {
          // "inifinity", just hold stroke somewhere >= 1.0
          states[STATE_STROKE] = 1.0;
        } else {
          //printf("fmodf(%f, %f) = ", (double)stroke, (double)wrap);
          states[STATE_STROKE] = fmodf(states[STATE_STROKE], wrap);
          //printf("%f\n", (double)stroke);
          assert(states[STATE_STROKE] >= 0);
        }
      }
    }

    // calculate final radius
    float radius_log;
    radius_log = settings_value[BRUSH_RADIUS_LOGARITHMIC];
    states[STATE_ACTUAL_RADIUS] = expf(radius_log);
    if (states[STATE_ACTUAL_RADIUS] < ACTUAL_RADIUS_MIN) states[STATE_ACTUAL_RADIUS] = ACTUAL_RADIUS_MIN;
    if (states[STATE_ACTUAL_RADIUS] > ACTUAL_RADIUS_MAX) states[STATE_ACTUAL_RADIUS] = ACTUAL_RADIUS_MAX;
  }

  // Called only from brush_stroke_to(). Calculate everything needed to
  // draw the dab, then let draw_brush_dab() do the actual drawing.
  //
  // This is always called "directly" after brush_update_settings_values.
  // Returns zero if nothing was drawn.
  int brush_prepare_and_draw_dab (RenderContext * rc)
  {
    float x, y, opaque;
    float radius;

    opaque = settings_value[BRUSH_OPAQUE] * settings_value[BRUSH_OPAQUE_MULTIPLY];
    if (opaque >= 1.0) opaque = 1.0;
    if (opaque <= 0.0) opaque = 0.0;
    //if (opaque == 0.0) return 0; <-- bad idea: need to update smudge state.
    if (settings_value[BRUSH_OPAQUE_LINEARIZE]) {
      // OPTIMIZE: no need to recalculate this for each dab
      float alpha, beta, alpha_dab, beta_dab;
      float dabs_per_pixel;
      // dabs_per_pixel is just estimated roughly, I didn't think hard
      // about the case when the radius changes during the stroke
      dabs_per_pixel = (
                        settings[BRUSH_DABS_PER_ACTUAL_RADIUS]->base_value + 
                        settings[BRUSH_DABS_PER_BASIC_RADIUS]->base_value
                        ) * 2.0;

      // the correction is probably not wanted if the dabs don't overlap
      if (dabs_per_pixel < 1.0) dabs_per_pixel = 1.0;

      // interpret the user-setting smoothly
      dabs_per_pixel = 1.0 + settings[BRUSH_OPAQUE_LINEARIZE]->base_value*(dabs_per_pixel-1.0);

      // see html/brushdab_saturation.png
      //      beta = beta_dab^dabs_per_pixel
      // <==> beta_dab = beta^(1/dabs_per_pixel)
      alpha = opaque;
      beta = 1.0-alpha;
      beta_dab = powf(beta, 1.0/dabs_per_pixel);
      alpha_dab = 1.0-beta_dab;
      opaque = alpha_dab;
    }

    x = states[STATE_ACTUAL_X];
    y = states[STATE_ACTUAL_Y];

    float base_radius = expf(settings[BRUSH_RADIUS_LOGARITHMIC]->base_value);

    if (settings_value[BRUSH_OFFSET_BY_SPEED]) {
      x += states[STATE_NORM_DX_SLOW] * settings_value[BRUSH_OFFSET_BY_SPEED] * 0.1 * base_radius;
      y += states[STATE_NORM_DY_SLOW] * settings_value[BRUSH_OFFSET_BY_SPEED] * 0.1 * base_radius;
    }

    if (settings_value[BRUSH_OFFSET_BY_RANDOM]) {
      x += rand_gauss (rng) * settings_value[BRUSH_OFFSET_BY_RANDOM] * base_radius;
      y += rand_gauss (rng) * settings_value[BRUSH_OFFSET_BY_RANDOM] * base_radius;
    }

  
    radius = states[STATE_ACTUAL_RADIUS];
    if (settings_value[BRUSH_RADIUS_BY_RANDOM]) {
      float radius_log, alpha_correction;
      // go back to logarithmic radius to add the noise
      radius_log  = settings_value[BRUSH_RADIUS_LOGARITHMIC];
      radius_log += rand_gauss (rng) * settings_value[BRUSH_RADIUS_BY_RANDOM];
      radius = expf(radius_log);
      if (radius < ACTUAL_RADIUS_MIN) radius = ACTUAL_RADIUS_MIN;
      if (radius > ACTUAL_RADIUS_MAX) radius = ACTUAL_RADIUS_MAX;
      alpha_correction = states[STATE_ACTUAL_RADIUS] / radius;
      alpha_correction = SQR(alpha_correction);
      if (alpha_correction <= 1.0) {
        opaque *= alpha_correction;
      }
    }

    // color part

    float color_h, color_s, color_v;
    float alpha_eraser;
    if (settings_value[BRUSH_SMUDGE] <= 0.0) {
      // normal case (do not smudge)
      color_h = settings[BRUSH_COLOR_H]->base_value;
      color_s = settings[BRUSH_COLOR_S]->base_value;
      color_v = settings[BRUSH_COLOR_V]->base_value;
      alpha_eraser = 1.0;
    } else if (settings_value[BRUSH_SMUDGE] >= 1.0) {
      // smudge only (ignore the original color)
      color_h = states[STATE_SMUDGE_R];
      color_s = states[STATE_SMUDGE_G];
      color_v = states[STATE_SMUDGE_B];
      rgb_to_hsv_float (&color_h, &color_s, &color_v);
      alpha_eraser = states[STATE_SMUDGE_A];
    } else {
      // mix (in RGB) the smudge color with the brush color
      color_h = settings[BRUSH_COLOR_H]->base_value;
      color_s = settings[BRUSH_COLOR_S]->base_value;
      color_v = settings[BRUSH_COLOR_V]->base_value;
      // XXX clamp??!? before this call?
      hsv_to_rgb_float (&color_h, &color_s, &color_v);
      float fac = settings_value[BRUSH_SMUDGE];
      color_h = (1-fac)*color_h + fac*states[STATE_SMUDGE_R];
      color_s = (1-fac)*color_s + fac*states[STATE_SMUDGE_G];
      color_v = (1-fac)*color_v + fac*states[STATE_SMUDGE_B];
      rgb_to_hsv_float (&color_h, &color_s, &color_v);
      alpha_eraser = (1-fac)*1.0 + fac*states[STATE_SMUDGE_A];
    }

    // update the smudge state
    if (settings_value[BRUSH_SMUDGE_LENGTH] < 1.0) {
      float fac = settings_value[BRUSH_SMUDGE_LENGTH];
      if (fac < 0.0) fac = 0;
      int px, py;
      px = ROUND(x);
      py = ROUND(y);
      float r, g, b, a;
      tile_get_color (rc->tiled_surface, px, py, /*radius*/5.0, &r, &g, &b, &a);
      states[STATE_SMUDGE_R] = fac*states[STATE_SMUDGE_R] + (1-fac)*r;
      states[STATE_SMUDGE_G] = fac*states[STATE_SMUDGE_G] + (1-fac)*g;
      states[STATE_SMUDGE_B] = fac*states[STATE_SMUDGE_B] + (1-fac)*b;
      states[STATE_SMUDGE_A] = fac*states[STATE_SMUDGE_A] + (1-fac)*a;
    }

    // HSV color change
    color_h += settings_value[BRUSH_CHANGE_COLOR_H];
    color_s += settings_value[BRUSH_CHANGE_COLOR_HSV_S];
    color_v += settings_value[BRUSH_CHANGE_COLOR_V];


    // HSL color change
    if (settings_value[BRUSH_CHANGE_COLOR_L] || settings_value[BRUSH_CHANGE_COLOR_HSL_S]) {
      // (calculating way too much here, can be optimized if neccessary)
      hsv_to_rgb_float (&color_h, &color_s, &color_v);
      rgb_to_hsl_float (&color_h, &color_s, &color_v);
      color_v += settings_value[BRUSH_CHANGE_COLOR_L];
      color_s += settings_value[BRUSH_CHANGE_COLOR_HSL_S];
      hsl_to_rgb_float (&color_h, &color_s, &color_v);
      rgb_to_hsv_float (&color_h, &color_s, &color_v);
    } 

    { // final calculations
      gint c[3];

      g_assert(opaque >= 0);
      g_assert(opaque <= 1);
    
      c[0] = ((int)(color_h*360.0)) % 360;
      if (c[0] < 0) c[0] += 360.0;
      g_assert(c[0] >= 0);
      c[1] = CLAMP(ROUND(color_s*255), 0, 255);
      c[2] = CLAMP(ROUND(color_v*255), 0, 255);

      hsv_to_rgb_int (c + 0, c + 1, c + 2);

      float hardness = settings_value[BRUSH_HARDNESS];
      if (hardness > 1.0) hardness = 1.0;
      if (hardness < 0.0) hardness = 0.0;

      return tile_draw_dab (rc, 
                            x, y, 
                            radius, 
                            c[0], c[1], c[2],
                            opaque, hardness
                            );
    }
  }

  // How many dabs will be drawn between the current and the next (x, y, pressure, +dt) position?
  float brush_count_dabs_to (float x, float y, float pressure, float dt)
  {
    float dx, dy;
    float res1, res2, res3;
    float dist;

    if (states[STATE_ACTUAL_RADIUS] == 0.0) states[STATE_ACTUAL_RADIUS] = expf(settings[BRUSH_RADIUS_LOGARITHMIC]->base_value);
    if (states[STATE_ACTUAL_RADIUS] < ACTUAL_RADIUS_MIN) states[STATE_ACTUAL_RADIUS] = ACTUAL_RADIUS_MIN;
    if (states[STATE_ACTUAL_RADIUS] > ACTUAL_RADIUS_MAX) states[STATE_ACTUAL_RADIUS] = ACTUAL_RADIUS_MAX;


    // OPTIMIZE: expf() called too often
    float base_radius = expf(settings[BRUSH_RADIUS_LOGARITHMIC]->base_value);
    if (base_radius < ACTUAL_RADIUS_MIN) base_radius = ACTUAL_RADIUS_MIN;
    if (base_radius > ACTUAL_RADIUS_MAX) base_radius = ACTUAL_RADIUS_MAX;
    //if (base_radius < 0.5) base_radius = 0.5;
    //if (base_radius > 500.0) base_radius = 500.0;

    dx = x - states[STATE_X];
    dy = y - states[STATE_Y];
    //dp = pressure - pressure; // Not useful?
    // TODO: control rate with pressure (dabs per pressure) (dpressure is useless)

    // OPTIMIZE
    dist = sqrtf (dx*dx + dy*dy);
    // FIXME: no need for base_value or for the range checks above IF always the interpolation
    //        function will be called before this one
    res1 = dist / states[STATE_ACTUAL_RADIUS] * settings[BRUSH_DABS_PER_ACTUAL_RADIUS]->base_value;
    res2 = dist / base_radius   * settings[BRUSH_DABS_PER_BASIC_RADIUS]->base_value;
    res3 = dt * settings[BRUSH_DABS_PER_SECOND]->base_value;
    return res1 + res2 + res3;
  }

  // Called from gtkmydrawwidget.c when a GTK event was received, with the new pointer position.
  void any_surface_stroke_to (RenderContext * rc, float x, float y, float pressure, double dtime)
  {
    //printf("%f %f %f %f\n", (double)dtime, (double)x, (double)y, (double)pressure);
    if (dtime <= 0) {
      if (dtime < 0) g_print("Time jumped backwards by dtime=%f seconds!\n", dtime);
      //g_print("timeskip  (dtime=%f)\n", dtime);
      return;
    }

    { // calculate the actual "virtual" cursor position

      // noise first
      if (settings[BRUSH_TRACKING_NOISE]->base_value) {
        // OPTIMIZE: expf() called too often
        float base_radius = expf(settings[BRUSH_RADIUS_LOGARITHMIC]->base_value);

        x += rand_gauss (rng) * settings[BRUSH_TRACKING_NOISE]->base_value * base_radius;
        y += rand_gauss (rng) * settings[BRUSH_TRACKING_NOISE]->base_value * base_radius;
      }

      float fac = 1.0 - exp_decay (settings[BRUSH_SLOW_TRACKING]->base_value, 100.0*dtime);
      x = states[STATE_X] + (x - states[STATE_X]) * fac;
      y = states[STATE_Y] + (y - states[STATE_Y]) * fac;
    }

    // draw many (or zero) dabs to the next position

    // see html/stroke2dabs.png
    float dist_moved = states[STATE_DIST];
    float dist_todo = brush_count_dabs_to (x, y, pressure, dtime);

    if (dtime > 5 || dist_todo > 300) {

      /*
        if (dist_todo > 300) {
        // this happens quite often, eg when moving the cursor back into the window
        // FIXME: bad to hardcode a distance treshold here - might look at zoomed image
        //        better detect leaving/entering the window and reset then.
        g_print ("Warning: NOT drawing %f dabs.\n", dist_todo);
        g_print ("dtime=%f, dx=%f\n", dtime, x-states[STATE_X]);
        //must_reset = 1;
        }
      */

      //printf("Brush reset.\n");
      memset(states, 0, sizeof(states[0])*STATE_COUNT);

      states[STATE_X] = x;
      states[STATE_Y] = y;
      states[STATE_PRESSURE] = pressure;

      // not resetting, because they will get overwritten below:
      //dx, dy, dpress, dtime

      states[STATE_ACTUAL_X] = states[STATE_X];
      states[STATE_ACTUAL_Y] = states[STATE_Y];
      states[STATE_STROKE] = 1.0; // start in a state as if the stroke was long finished
      dtime = 0.0001; // not sure if it this is needed

      split_stroke ();
      return; // ?no movement yet?
    }

    //g_print("dist = %f\n", states[STATE_DIST]);
    enum { UNKNOWN, YES, NO } painted = UNKNOWN;
    double dtime_left = dtime;

    while (dist_moved + dist_todo >= 1.0) { // there are dabs pending
      { // linear interpolation (nonlinear variant was too slow, see SVN log)
        float frac; // fraction of the remaining distance to move
        if (dist_moved > 0) {
          // "move" the brush exactly to the first dab (moving less than one dab)
          frac = (1.0 - dist_moved) / dist_todo;
          dist_moved = 0;
        } else {
          // "move" the brush from one dab to the next
          frac = 1.0 / dist_todo;
        }
        dx        = frac * (x - states[STATE_X]);
        dy        = frac * (y - states[STATE_Y]);
        dpressure = frac * (pressure - states[STATE_PRESSURE]);
        dtime     = frac * (dtime_left - 0.0);
        // Though it looks different, time is interpolated exactly like x/y/pressure.
      }
    
      states[STATE_X]        += dx;
      states[STATE_Y]        += dy;
      states[STATE_PRESSURE] += dpressure;

      brush_update_settings_values ();
      int painted_now = brush_prepare_and_draw_dab (rc);
      if (painted_now) {
        painted = YES;
      } else if (painted == UNKNOWN) {
        painted = NO;
      }

      dtime_left   -= dtime;
      dist_todo  = brush_count_dabs_to (x, y, pressure, dtime_left);
    }

    {
      // "move" the brush to the current time (no more dab will happen)
      // Important to do this at least once every event, because
      // brush_count_dabs_to depends on the radius and the radius can
      // depend on something that changes much faster than only every
      // dab (eg speed).
    
      dx        = x - states[STATE_X];
      dy        = y - states[STATE_Y];
      dpressure = pressure - states[STATE_PRESSURE];
      dtime     = dtime_left;
    
      states[STATE_X] = x;
      states[STATE_Y] = y;
      states[STATE_PRESSURE] = pressure;
      //dtime_left = 0; but that value is not used any more

      brush_update_settings_values ();
    }

    // save the fraction of a dab that is already done now
    states[STATE_DIST] = dist_moved + dist_todo;
    //g_print("dist_final = %f\n", states[STATE_DIST]);

    if (rc->bbox.w > 0) {
      Rect bbox = rc->bbox;
      ExpandRectToIncludePoint(&stroke_bbox, bbox.x, bbox.y);
      ExpandRectToIncludePoint(&stroke_bbox, bbox.x+bbox.w-1, bbox.y+bbox.h-1);
    }

    // stroke separation logic

    if (painted == UNKNOWN) {
      if (stroke_idling_time > 0) {
        // still idling
        painted = NO;
      } else {
        // probably still painting (we get more events than brushdabs)
        painted = YES;
        //if (pressure == 0) g_print ("info: assuming 'still painting' while there is no pressure\n");
      }
    }
    if (painted == YES) {
      //if (stroke_idling_time > 0) g_print ("idling ==> painting\n");
      stroke_total_painting_time += dtime;
      stroke_idling_time = 0;
      // force a stroke split after some time
      if (stroke_total_painting_time > 5 + 10*pressure) {
        // but only if pressure is not being released
        if (dpressure >= 0) {
          split_stroke ();
        }
      }
    } else if (painted == NO) {
      //if (stroke_idling_time == 0) g_print ("painting ==> idling\n");
      stroke_idling_time += dtime;
      if (stroke_total_painting_time == 0) {
        // not yet painted, split to discard the useless motion data
        g_assert (stroke_bbox.w == 0);
        if (stroke_idling_time > 1.0) {
          split_stroke ();
        }
      } else {
        // Usually we have pressure==0 here. But some brushes can paint
        // nothing at full pressure (eg gappy lines, or a stroke that
        // fades out). In either case this is the prefered moment to split.
        if (stroke_total_painting_time+stroke_idling_time > 1.5 + 5*pressure) {
          split_stroke ();
        }
      }
    }
  }

  PyObject * tiled_surface_stroke_to (PyObject * tiled_surface, float x, float y, float pressure, double dtime)
  {
    RenderContext rc;
    rc.tiled_surface = tiled_surface;
    rc.bbox.w = 0;
    any_surface_stroke_to (&rc, x, y, pressure, dtime);
    if (exception) {
      exception = false;
      return NULL;
    }
    if (rc.bbox.w == 0) {
      Py_RETURN_NONE;
    } else {
      return Py_BuildValue("(iiii)", rc.bbox.x, rc.bbox.y, rc.bbox.w, rc.bbox.h);
    }
  }

  void set_split_stroke_callback (PyObject * callback)
  {
    Py_CLEAR (split_stroke_callback);
    Py_INCREF (callback);
    split_stroke_callback = callback;
  }

  void split_stroke ()
  {
    if (split_stroke_callback) {
      PyObject * arglist = Py_BuildValue("()");
      PyObject * result = PyEval_CallObject(split_stroke_callback, arglist);
      Py_DECREF (arglist);
      if (!result) {
        printf("Exception during split_stroke callback!\n");
        exception = true;
      } else {
        Py_DECREF (result);
      }
    }

    stroke_idling_time = 0;
    stroke_total_painting_time = 0;

    stroke_bbox.w = 0;
    stroke_bbox.h = 0;
    stroke_bbox.x = 0;
    stroke_bbox.y = 0;
  }

private:
  // TODO: move this out of the brush class!
#define SIZE 256
  struct PrecalcData {
    int h;
    int s;
    int v;
    //signed char s;
    //signed char v;
  };

  PrecalcData * precalcData[4];
  int precalcDataIndex;

  PrecalcData * precalc_data(float phase0)
  {
    // Hint to the casual reader: some of the calculation here do not
    // what I originally intended. Not everything here will make sense.
    // It does not matter in the end, as long as the result looks good.

    int width, height;
    float width_inv, height_inv;
    int x, y, i;
    PrecalcData * result;

    width = SIZE;
    height = SIZE;
    result = (PrecalcData*)g_malloc(sizeof(PrecalcData)*width*height);

    //phase0 = rand_double (rng) * 2*M_PI;

    width_inv = 1.0/width;
    height_inv = 1.0/height;

    i = 0;
    for (y=0; y<height; y++) {
      for (x=0; x<width; x++) {
        float h, s, v, s_original, v_original;
        int dx, dy;
        float v_factor = 0.8;
        float s_factor = 0.8;
        float h_factor = 0.05;

#define factor2_func(x) ((x)*(x)*SIGN(x))
        float v_factor2 = 0.01;
        float s_factor2 = 0.01;


        h = 0;
        s = 0;
        v = 0;

        dx = x-width/2;
        dy = y-height/2;

        // basically, its x-axis = value, y-axis = saturation
        v = dx*v_factor + factor2_func(dx)*v_factor2;
        s = dy*s_factor + factor2_func(dy)*s_factor2;

        v_original = v; s_original = s;

        // overlay sine waves to color hue, not visible at center, ampilfying near the border
        if (1) {
          float amplitude, phase;
          float dist, dist2, borderdist;
          float dx_norm, dy_norm;
          float angle;
          dx_norm = dx*width_inv;
          dy_norm = dy*height_inv;

          dist2 = dx_norm*dx_norm + dy_norm*dy_norm;
          dist = sqrtf(dist2);
          borderdist = 0.5 - MAX(ABS(dx_norm), ABS(dy_norm));
          angle = atan2f(dy_norm, dx_norm);
          amplitude = 50 + dist2*dist2*dist2*100;
          phase = phase0 + 2*M_PI* (dist*0 + dx_norm*dx_norm*dy_norm*dy_norm*50) + angle*7;
          //h = sinf(phase) * amplitude;
          h = sinf(phase);
          h = (h>0)?h*h:-h*h;
          h *= amplitude;

          // calcualte angle to next 45-degree-line
          angle = ABS(angle)/M_PI;
          if (angle > 0.5) angle -= 0.5;
          angle -= 0.25;
          angle = ABS(angle) * 4;
          // angle is now in range 0..1
          // 0 = on a 45 degree line, 1 = on a horizontal or vertical line

          v = 0.6*v*angle + 0.4*v;
          h = h * angle * 1.5;
          s = s * angle * 1.0;

          // this part is for strong color variations at the borders
          if (borderdist < 0.3) {
            float fac;
            float h_new;
            fac = (1 - borderdist/0.3);
            // fac is 1 at the outermost pixels
            v = (1-fac)*v + fac*0;
            s = (1-fac)*s + fac*0;
            fac = fac*fac*0.6;
            h_new = (angle+phase0+M_PI/4)*360/(2*M_PI) * 8;
            while (h_new > h + 360/2) h_new -= 360;
            while (h_new < h - 360/2) h_new += 360;
            h = (1-fac)*h + fac*h_new;
            //h = (angle+M_PI/4)*360/(2*M_PI) * 4;
          }
        }

        {
          // undo that funky stuff on horizontal and vertical lines
          int min = ABS(dx);
          if (ABS(dy) < min) min = ABS(dy);
          if (min < 30) {
            float mul;
            min -= 6;
            if (min < 0) min = 0;
            mul = min / (30.0-1.0-6.0);
            h = mul*h; //+ (1-mul)*0;

            v = mul*v + (1-mul)*v_original;
            s = mul*s + (1-mul)*s_original;
          }
        }

        h -= h*h_factor;

        result[i].h = (int)h;
        result[i].v = (int)v;
        result[i].s = (int)s;
        i++;
      }
    }
    return result;
  }

public:
  GdkPixbuf* get_colorselection_pixbuf ()
  {
    GdkPixbuf* pixbuf;
    PrecalcData * pre;
    guchar * pixels;
    int rowstride, n_channels;
    int x, y;
    int h, s, v;
    int base_h, base_s, base_v;

    pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, /*has_alpha*/0, /*bits_per_sample*/8, SIZE, SIZE);

    pre = precalcData[precalcDataIndex];
    if (!pre) {
      pre = precalcData[precalcDataIndex] = precalc_data(2*M_PI*(precalcDataIndex/4.0));
    }
    precalcDataIndex++;
    precalcDataIndex %= 4;

    n_channels = gdk_pixbuf_get_n_channels (pixbuf);
    g_assert (!gdk_pixbuf_get_has_alpha (pixbuf));
    g_assert (n_channels == 3);

    rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    pixels = gdk_pixbuf_get_pixels (pixbuf);

    base_h = settings[BRUSH_COLOR_H]->base_value*360;
    base_s = settings[BRUSH_COLOR_S]->base_value*255;
    base_v = settings[BRUSH_COLOR_V]->base_value*255;

    for (y=0; y<SIZE; y++) {
      for (x=0; x<SIZE; x++) {
        guchar * p;

        h = base_h + pre->h;
        s = base_s + pre->s;
        v = base_v + pre->v;
        pre++;


        if (s < 0) { if (s < -50) { s = - (s + 50); } else { s = 0; } } 
        if (s > 255) { if (s > 255 + 50) { s = 255 - ((s-50)-255); } else { s = 255; } }
        if (v < 0) { if (v < -50) { v = - (v + 50); } else { v = 0; } }
        if (v > 255) { if (v > 255 + 50) { v = 255 - ((v-50)-255); } else { v = 255; } }

        s = s & 255;
        v = v & 255;
        h = h%360; if (h<0) h += 360;

        p = pixels + y * rowstride + x * n_channels;
        hsv_to_rgb_int (&h, &s, &v);
        p[0] = h; p[1] = s; p[2] = v;
      }
    }
    return pixbuf;
  }

  double random_double ()
  {
    return g_rand_double (rng);
  }

  void srandom (int value)
  {
    g_rand_set_seed (rng, value);
  }

  GString* get_state ()
  {
    // see also mydrawwidget.override
    int i;
    GString * bs = g_string_new ("1"); // version id
    for (i=0; i<STATE_COUNT; i++) {
      BS_WRITE_FLOAT (states[i]);
    }

    return bs;
  }

  void set_state (GString * data)
  {
    // see also mydrawwidget.override
    char * p = data->str;
    char c;

    BS_READ_CHAR (c);
    if (c != '1') {
      g_print ("Unknown state version ID\n");
      return;
    }

    memset(states, 0, sizeof(states[0])*STATE_COUNT);
    int i = 0;
    while (p<data->str+data->len && i < STATE_COUNT) {
      BS_READ_FLOAT (states[i]);
      i++;
      //g_print ("states[%d] = %f\n", i, states[i]);
    }
  }
};
