/*
 * BADGE.TEAM framebuffer driver
 * Uses parts of the Adafruit GFX Arduino libray
 */

/*
This is the core graphics library for all our displays, providing a common
set of graphics primitives (points, lines, circles, etc.).  It needs to be
paired with a hardware-specific library for each display device we carry
(to handle the lower-level functions).

Adafruit invests time and resources providing this open source code, please
support Adafruit & open-source hardware by purchasing products from Adafruit!

Copyright (c) 2013 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
 */

#include "include/driver_framebuffer_internal.h"

#include <math.h>

#define TAG "fb-drawing"

#ifdef CONFIG_DRIVER_FRAMEBUFFER_ENABLE

typedef struct texture_2d_t {
	uint32_t *buffer;
	int16_t width;
	int16_t height;
} texture_2d;

typedef struct point_3d_t {
	double x, y, z;
} point_3d;

typedef struct triangle_3d_t {
	bool modificationAllowed; //This is false if the shape is being drawn, because at this time it will not affect anything to modify vertices.
	point_3d *point0;
	point_3d *point1;
	point_3d *point2;
	double u0, v0;
	double u1, v1;
	double u2, v2;
} triangle_3d;

/*
 * Defines a shader applicable to 2D drawing.
 * A shader is a piece of code which allows for special coloring of a drawn object in an otherwise impossible way.
 * 
 * Arguments:
 * uint32_t tint -- Tint of the drawing, named tint because this would be used to tint the texture, or specify the color if no texture is present.
 * texture_2d *texture -- Pointer to the texture object, if NULL then there is no texture.
 * int16_t screenX -- Real X position on screen.
 * int16_t screenY -- Real Y position on screen.
 * double preTransformX -- X position on screen that this would have been it if it were untransformed.
 * double preTransformY -- Y position on screen that this would have been it if it were untransformed.
 * double u -- Represents the X coordinate on the texture, from 0 to 1.
 * double v -- Represents the Y coordinate on the texture, from 0 to 1.
 * void *args -- Anything as an argument to the shader.
 * int n_args -- How many arguments the shader has recieved.
 * 
 * Returns:
 * The color to be drawn, formatted in 0xAARRGGBB.
 */
typedef uint32_t(*shader_2d)(uint32_t tint, texture_2d *texture, int16_t screenX, int16_t screenY, \
				double preTransformX, double preTransformY, double u, double v, void *args, int n_args);

/*
 * Defines a shader applicable to 2D drawing.
 * A shader is a piece of code which allows for special coloring of a drawn object in an otherwise impossible way.
 * 3D shaders also have the power to distort an object, such as moving vertices around.
 * For this reason, the shader will be called twice:
 * Once to displace the vertices,
 * And once more to draw the shape or model.
 * 
 * Arguments:
 * uint32_t tint -- Tint of the drawing, named tint because this would be used to tint the texture, or specify the color if no texture is present.
 * texture_2d *texture -- Pointer to the texture object, if NULL then there is no texture.
 * int16_t screenX -- Real X position on screen.
 * int16_t screenY -- Real Y position on screen.
 * double preTransformX -- X position in space that this would have been it if it were untransformed.
 * double preTransformY -- Y position in space that this would have been it if it were untransformed.
 * double preTransformZ -- Z position in space that this would have been it if it were untransformed.
 * double u -- Represents the X coordinate on the texture, from 0 to 1.
 * double v -- Represents the Y coordinate on the texture, from 0 to 1.
 * triangle_3d *triangle -- The triangle upon which this shader is applied, it is a pointer to allow modifiction of the triangle.
 * void *args -- Anything as an argument to the shader.
 * int n_args -- How many arguments the shader has recieved.
 * 
 * Returns:
 * The color to be drawn, formatted in 0xAARRGGBB.
 */
typedef uint32_t(*shader_3d)(uint32_t tint, texture_2d *texture, int16_t screenX, int16_t screenY, \
				double preTransformX, double preTransformY, double preTransformZ, double u, double v, triangle_3d *triangle, void *args, int n_args);

#define _swap_int16_t(a, b) { int16_t t = a; a = b; b = t; }

void driver_framebuffer_line(Window* window, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color)
{
	int16_t steep = abs(y1 - y0) > abs(x1 - x0);
	if (steep) {
		_swap_int16_t(x0, y0);
		_swap_int16_t(x1, y1);
	}

	if (x0 > x1) {
		_swap_int16_t(x0, x1);
		_swap_int16_t(y0, y1);
	}

	int16_t dx, dy;
	dx = x1 - x0;
	dy = abs(y1 - y0);

	int16_t err = dx / 2;
	int16_t ystep;

	if (y0 < y1) {
		ystep = 1;
	} else {
		ystep = -1;
	}

	for (/*empty*/; x0<=x1; x0++) {
		if (steep) {
			driver_framebuffer_setPixel(window, y0, x0, color);
		} else {
			driver_framebuffer_setPixel(window, x0, y0, color);
		}
		err -= dy;
		if (err < 0) {
			y0 += ystep;
			err += dx;
		}
	}
}

void driver_framebuffer_triangle(Window* window, double x0, double y0, double x1, double y1, double x2, double y2, uint32_t color)
{
	//sort the points such that point 0 is the top and point 2 is the bottom
	//lower number is higher on screen
	float temp;
	if (y1 < y0) { //ensure y1 is under y0
		//swap points 1 and 0
		temp = y0;
		y0 = y1;
		y1 = temp;
		temp = x0;
		x0 = x1;
		x1 = temp;
	}
	if (y2 < y1) { //ensure y2 is under y1
		//swap points 2 and 1
		temp = y1;
		y1 = y2;
		y2 = temp;
		temp = x1;
		x1 = x2;
		x2 = temp;
	}
	if (y2 < y0) { //ensure y2 is under y0
		//swap points 2 and 0
		temp = y0;
		y0 = y2;
		y2 = temp;
		temp = x0;
		x0 = x2;
		x2 = temp;
	}
	if (y1 < y0) { //ensure y1 is under y0 once more
		//swap points 1 and 0
		temp = y0;
		y0 = y1;
		y1 = temp;
		temp = x0;
		x0 = x1;
		x1 = temp;
	}
	
	double yDist = y1 - y0; //between points 0 and 1
	int nSteps = (int) (yDist + 0.9999); //between points 0 and 1
	double yStep = yDist / (double) nSteps; //between points 0 and 1
	double xMiddle = x0 + (x2 - x0) / (y2 - y0) * (y1 - y0);
	double xStep0 = (xMiddle - x0) / (double) nSteps; //between points 0 and 2
	double xStep1 = (x1 - x0) / (double) nSteps; //between points 0 and 1
	//"top" part of the triangle
	for (int i = 0; i < nSteps; i++) { //go along the rows
		int y = (int) (y0 + yStep * i + 0.5);
		int xC0 = (int) (x0 + xStep0 * i + 0.5);
		int xC1 = (int) (x0 + xStep1 * i + 0.5);
		int nXSteps = xC0 - xC1;
		int xStepMult = -1;
		if (nXSteps < 0) {
			nXSteps = -nXSteps;
			xStepMult = 1;
		}
		for (int j = 0; j < nXSteps; j++) { //and plot each pixel that falls in it
			driver_framebuffer_setPixel(window, xC0 + j * xStepMult, y, color);
		}
	}

	yDist = y2 - y1; //between points 1 and 2
	nSteps = (int) (yDist + 0.9999); //between points 1 and 2
	yStep = yDist / (double) nSteps + 0.01; //between points 1 and 2
	xStep0 = (x2 - xMiddle) / (double) nSteps; //between points 0 and 2
	xStep1 = (x2 - x1) / (double) nSteps; //between points 1 and 2
	//"bottom" part of the triangle
	for (int i = 0; i < nSteps; i++) { //go along the rows
		int y = (int) (y1 + yStep * i + 0.5);
		int xC0 = (int) (xMiddle + xStep0 * i + 0.5);
		int xC1 = (int) (x1 + xStep1 * i + 0.5);
		int nXSteps = xC0 - xC1;
		int xStepMult = -1;
		if (nXSteps < 0) {
			nXSteps = -nXSteps;
			xStepMult = 1;
		}
		for (int j = 0; j < nXSteps; j++) { //and plot each pixel that falls in it
			driver_framebuffer_setPixel(window, xC0 + j * xStepMult, y, color);
		}
	}
}

void driver_framebuffer_quad(Window* window, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, uint32_t color)
{
	// This is easier to do if represented as triangles.
	driver_framebuffer_triangle(window, x0, y0, x1, y1, x2, y2, color);
	driver_framebuffer_triangle(window, x1, y1, x3, y3, x2, y2, color);
	// Do this twice so as to make a gap less likely.
	driver_framebuffer_triangle(window, x0, y0, x1, y1, x3, y3, color);
	driver_framebuffer_triangle(window, x1, y1, x2, y2, x3, y3, color);
	// Unfortunately, very thin quads will still show a gap occasionally.
}

void driver_framebuffer_rect(Window* window, int16_t x, int16_t y, uint16_t w, uint16_t h, bool fill, uint32_t color)
{
	if (fill) {
		for (int16_t i=x; i<x+w; i++) {
			driver_framebuffer_line(window, i, y, i, y+h-1, color);
		}
	} else {
		driver_framebuffer_line(window, x,    y,     x+w-1, y,     color);
		driver_framebuffer_line(window, x,    y+h-1, x+w-1, y+h-1, color);
		driver_framebuffer_line(window, x,    y,     x,     y+h-1, color);
		driver_framebuffer_line(window, x+w-1,y,     x+w-1, y+h-1, color);
	}
}


void driver_framebuffer_circle(Window* window, int16_t x0, int16_t y0, uint16_t r, uint16_t startAngle, uint16_t endAngle, bool fill, uint32_t color)
{
	bool havePrevPixel = 0;
	int prevX = 0;
	int prevY = 0;
	if (startAngle >= endAngle) return;
	for (int f=(fill ? 0 : r); f <= r; f++) {
		havePrevPixel = false;
		for (int i=startAngle; i<endAngle; i++) {
			double radians = i * M_PI / 180;
			int px = x0 + f * cos(radians);
			int py = y0 + f * sin(radians);
			if (havePrevPixel && ((prevX != px) || (prevY != py))) {
				driver_framebuffer_line(window, prevX, prevY, px, py, color);
			} else {
				driver_framebuffer_setPixel(window, px, py, color);
			}
			prevX = px;
			prevY = py;
			havePrevPixel = true;
		}
	}
}

#endif
