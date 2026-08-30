/*
 * Copyright (C) 2001-2010  Terence M. Welsh
 *
 * This file is part of Implicit.
 *
 * Implicit is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Implicit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef IMPCUBETABLE_H
#define IMPCUBETABLE_H

/****************************************

OpenGL coordinate system

	  Y
	  |
	  |
	  |
	  |
	  |
	  /--------X
	 /
	/
   Z

Indices for vertices and edge on cube

	   2-----6-----6
	  /|          /|
	 3 |        11 |
	/  1        /  9
   3-----7-----7   |
   |   |       |   |
   |   0-----4-|---4
   2  /       10  /
   | 0         | 8
   |/          |/
   1-----5-----5

*****************************************/

// corners
// left-right-bottom-top-far-near notation
#define LBF 0x01
#define LBN 0x02
#define LTF 0x04
#define LTN 0x08
#define RBF 0x10
#define RBN 0x20
#define RTF 0x40
#define RTN 0x80

class impCubeTables
{
public:
	// This table describes the sequence of edges to visit
	// in order to build triangle strips in a cube  There are
	// a maximum of 16 integers that can result from a cube
	// configuration.  We have 17 integers in each row so that
	// each row can end with a 0.  The zero signifies the end
	// of the data in each row.
	//int** cubetable;
	int triStripPatterns[256][17];

	// 256 x 6 array of true/false values.  For each of the 256
	// entries in the cubetable, this table indicates which
	// neighboring cubes will also contain parts of the surface.
	//bool** crawltable;
	bool crawlDirections[256][6];

	impCubeTables();
	~impCubeTables() {};

private:
	// edge connectivity
	// This array defines which vertices are connected by each edge.
	int ec[12][2];

	// vertex connectivity
	// This array defines which edges extend from each vertex.
	int vc[8][3];

	int nextedge(int vertex, int edge);
	void addtotable(int row, int edgecount, int* edgelist);
	void makeTriStripPatterns();
	void makeCrawlDirections();
};

#endif
