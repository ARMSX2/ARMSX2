/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Skyrocket.
 *
 * Skyrocket is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Skyrocket is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef SMOKE_H
#define SMOKE_H



#include "gl1.h"



#define SMOKETIMES 8
#define WHICHSMOKES 100


extern int dSmoke;
extern int dExplosionsmoke;
// lifespans for smoke particles
extern float smokeTime[SMOKETIMES];  // lifespans of consecutive smoke particles
extern int whichSmoke[WHICHSMOKES];  // table to indicate which particles produce smoke
// smoke display lists
extern unsigned int smokelist[5];



// Initialize smoke texture objects and display lists
void initSmoke();



#endif  // SMOKE_H
