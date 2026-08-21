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

#include "impCubeTables.h"

impCubeTables::impCubeTables()
{
	ec[0][0] = 0;
	ec[0][1] = 1;
	ec[1][0] = 0;
	ec[1][1] = 2;
	ec[2][0] = 1;
	ec[2][1] = 3;
	ec[3][0] = 2;
	ec[3][1] = 3;
	ec[4][0] = 0;
	ec[4][1] = 4;
	ec[5][0] = 1;
	ec[5][1] = 5;
	ec[6][0] = 2;
	ec[6][1] = 6;
	ec[7][0] = 3;
	ec[7][1] = 7;
	ec[8][0] = 4;
	ec[8][1] = 5;
	ec[9][0] = 4;
	ec[9][1] = 6;
	ec[10][0] = 5;
	ec[10][1] = 7;
	ec[11][0] = 6;
	ec[11][1] = 7;

	vc[0][0] = 0;
	vc[0][1] = 1;
	vc[0][2] = 4;
	vc[1][0] = 0;
	vc[1][1] = 5;
	vc[1][2] = 2;
	vc[2][0] = 1;
	vc[2][1] = 3;
	vc[2][2] = 6;
	vc[3][0] = 2;
	vc[3][1] = 7;
	vc[3][2] = 3;
	vc[4][0] = 4;
	vc[4][1] = 9;
	vc[4][2] = 8;
	vc[5][0] = 5;
	vc[5][1] = 8;
	vc[5][2] = 10;
	vc[6][0] = 6;
	vc[6][1] = 11;
	vc[6][2] = 9;
	vc[7][0] = 7;
	vc[7][1] = 10;
	vc[7][2] = 11;

	makeTriStripPatterns();
	makeCrawlDirections();
}

// determines next edge extending from a vertex in counterclockwise order
int
impCubeTables::nextedge(int vertex, int edge)
{
	if (vc[vertex][0] == edge)
		return vc[vertex][1];
	if (vc[vertex][1] == edge)
		return vc[vertex][2];
	if (vc[vertex][2] == edge)
		return vc[vertex][0];

	return -1;
}

// adds a triangle strip description to the cube table
void
impCubeTables::addtotable(int row, int edgecount, int* edgelist)
{
	static int lastrow = -1;
	static int totalcount;

	if (row < 0 || row >= 256)
		return;

	if (row != lastrow)
		totalcount = 0;

	// enter the number of vertices into the cubetable
	triStripPatterns[row][totalcount] = edgecount;

	// The edges are listed in counterclockwise order in the edgelist.
	// Notice how they are added to the cubetable out of order, in
	// a zig-zag pattern the way a triangle strip is drawn.
	// There can be at most 7 vertices in one of these triangle strips.
	switch (edgecount)
	{
		case 3:
			triStripPatterns[row][totalcount + 1] = edgelist[0];
			triStripPatterns[row][totalcount + 2] = edgelist[1];
			triStripPatterns[row][totalcount + 3] = edgelist[2];
			break;
		case 4:
			triStripPatterns[row][totalcount + 1] = edgelist[0];
			triStripPatterns[row][totalcount + 2] = edgelist[1];
			triStripPatterns[row][totalcount + 3] = edgelist[3];
			triStripPatterns[row][totalcount + 4] = edgelist[2];
			break;
		case 5:
			triStripPatterns[row][totalcount + 1] = edgelist[0];
			triStripPatterns[row][totalcount + 2] = edgelist[1];
			triStripPatterns[row][totalcount + 3] = edgelist[4];
			triStripPatterns[row][totalcount + 4] = edgelist[2];
			triStripPatterns[row][totalcount + 5] = edgelist[3];
			break;
		case 6:
			triStripPatterns[row][totalcount + 1] = edgelist[0];
			triStripPatterns[row][totalcount + 2] = edgelist[1];
			triStripPatterns[row][totalcount + 3] = edgelist[5];
			triStripPatterns[row][totalcount + 4] = edgelist[2];
			triStripPatterns[row][totalcount + 5] = edgelist[4];
			triStripPatterns[row][totalcount + 6] = edgelist[3];
			break;
		case 7:
			triStripPatterns[row][totalcount + 1] = edgelist[0];
			triStripPatterns[row][totalcount + 2] = edgelist[1];
			triStripPatterns[row][totalcount + 3] = edgelist[6];
			triStripPatterns[row][totalcount + 4] = edgelist[2];
			triStripPatterns[row][totalcount + 5] = edgelist[5];
			triStripPatterns[row][totalcount + 6] = edgelist[3];
			triStripPatterns[row][totalcount + 7] = edgelist[4];
			break;
	}

	totalcount += (edgecount + 1);
	lastrow = row;
}

void
impCubeTables::makeTriStripPatterns()
{
	int i, j, k;
	int currentvertex;
	int currentedge;

	bool vertices[8];  // true if on low side of gradient (outside of surface)
	bool edges[12];
	bool edgesdone[12];

	int edgelist[7];  // final list of egdes used in a triangle strip
	int edgecount;

	// Set cubetable values to zero
	// A zero will indicate that there are no more triangle strips to build
	for (i = 0; i < 256; i++)
	{
		for (j = 0; j < 17; j++)
			triStripPatterns[i][j] = 0;
	}

	// For each vertex combination
	for (i = 0; i < 256; i++)
	{
		// identify the vertices on the low side of the gradient
		int andbit;
		for (j = 0; j < 8; j++)
		{
			andbit = 1;
			for (k = 0; k < j; k++)
				andbit *= 2;
			if (i & andbit)
				vertices[j] = 1;
			else
				vertices[j] = 0;
		}

		// Identify the edges that cross threshold value
		// These are edges that connect 1 turned-on and 1 turned-off vertex
		for (j = 0; j < 12; j++)
		{
			if ((vertices[ec[j][0]] + vertices[ec[j][1]]) == 1)
				edges[j] = 1;
			else
				edges[j] = 0;

			edgesdone[j] = 0;  // no edges have been used yet
		}

		// Construct lists of edges that form triangle strips
		// try starting from each edge (no need to try last 2 edges)
		for (j = 0; j < 10; j++)
		{
			currentedge = j;
			edgecount = 0;

			// if this edge contains a surface vertex and hasn't been used
			while (edges[currentedge] && !edgesdone[currentedge])
			{
				// add edge to list
				edgelist[edgecount] = currentedge;
				edgecount++;
				edgesdone[currentedge] = 1;
				// find that edge's vertex on low side of gradient
				if (vertices[ec[currentedge][0]])
					currentvertex = ec[currentedge][0];
				else
					currentvertex = ec[currentedge][1];
				// move along gradiant boundary to find next edge
				currentedge = nextedge(currentvertex, currentedge);
				while (!edges[currentedge])
				{
					if (currentvertex != ec[currentedge][0])
						currentvertex = ec[currentedge][0];
					else
						currentvertex = ec[currentedge][1];
					currentedge = nextedge(currentvertex, currentedge);
				}
			}

			// if a surface has been created add it to the table
			// and start over to try to make another surface
			if (edgecount)
				addtotable(i, edgecount, edgelist);
		}
	}
}

void
impCubeTables::makeCrawlDirections()
{
	int i, j, k;
	bool vertices[8];  // vertices below gradient threshold get turned on
	bool edges[12];  // edges that cross gradient threshold get turned on

	// For each vertex combination
	for (i = 0; i < 256; i++)
	{
		// identify the vertices on the low side of the gradient
		int andbit;
		for (j = 0; j < 8; j++)
		{
			andbit = 1;
			for (k = 0; k < j; k++)
				andbit *= 2;
			if (i & andbit)
				vertices[j] = 1;
			else
				vertices[j] = 0;
		}

		// Identify the edges that cross threshold value
		// These are edges that connect 1 turned-on and 1 turned-off vertex
		for (j = 0; j < 12; j++)
		{
			if ((vertices[ec[j][0]] + vertices[ec[j][1]]) == 1)
				edges[j] = 1;
			else
				edges[j] = 0;
		}

		// -x
		if (edges[0] || edges[1] || edges[2] || edges[3])
			crawlDirections[i][0] = true;
		else
			crawlDirections[i][0] = false;

		// +x
		if (edges[8] || edges[9] || edges[10] || edges[11])
			crawlDirections[i][1] = true;
		else
			crawlDirections[i][1] = false;

		// -y
		if (edges[0] || edges[4] || edges[5] || edges[8])
			crawlDirections[i][2] = true;
		else
			crawlDirections[i][2] = false;

		// +y
		if (edges[3] || edges[6] || edges[7] || edges[11])
			crawlDirections[i][3] = true;
		else
			crawlDirections[i][3] = false;

		// -z
		if (edges[1] || edges[4] || edges[6] || edges[9])
			crawlDirections[i][4] = true;
		else
			crawlDirections[i][4] = false;

		// +z
		if (edges[2] || edges[5] || edges[7] || edges[10])
			crawlDirections[i][5] = true;
		else
			crawlDirections[i][5] = false;
	}
}
