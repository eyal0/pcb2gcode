/*
 * This file is part of pcb2gcode.
 * 
 * Copyright (C) 2015 Nicola Corna <nicola@corna.info>
 *
 * pcb2gcode is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * pcb2gcode is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with pcb2gcode.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TSP_HPP
#define TSP_HPP

#include <vector>
using std::vector;

#include <list>
using std::list;

#include <memory>
using std::shared_ptr;

#include "geometry.hpp"
using std::pair;

using std::next;

class tsp_solver
{
private:
    // You can extend this class adding new overloads of get with this prototype:
    //  icoordpair get(T _name_) { ... }
    static inline icoordpair get(icoordpair point)
    {
        return point;
    }

    static inline icoordpair get(shared_ptr<icoords> path)
    {
        return path->front();
    }

public:
    // This function computes the optimised path of a
    //  * icoordpair
    //  * shared_ptr<icoords>
    // In the case of icoordpair it interprets the coordpairs as coordinates and computes the optimised path
    // In the case of shared_ptr<icoords> it interprets the vector<icoordpair> as closed paths, and it computes
    // the optimised path of the first point of each subpath. This can be used in the milling paths, where each
    // subpath is closed and we want to find the best subpath order
    template <typename T>
    static void nearest_neighbour(vector<T> &path, icoordpair startingPoint, double quantization_error)
    {
        if (path.size() > 0)
        {
            list<T> temp_path (path.begin(), path.end());
            vector<T> newpath;
            double original_length;
            double new_length;
            double minDistance;
            unsigned int size = path.size();

            //Reserve memory
            newpath.reserve(size);

            new_length = 0;

            //Find the original path length
            original_length = boost::geometry::distance(startingPoint, get(temp_path.front()));
            for (auto point = temp_path.begin(); next(point) != temp_path.end(); point++)
                original_length += boost::geometry::distance(get(*point), get(*next(point)));

            icoordpair currentPoint = startingPoint;
            while (temp_path.size() > 1)
            {

                minDistance = boost::geometry::comparable_distance(currentPoint, get(*(temp_path.begin())));
                auto nearestPoint = temp_path.begin();
                //Compute all the distances
                for (auto i = temp_path.begin(); i != temp_path.end(); i++) {
                    if (boost::geometry::comparable_distance(currentPoint, get(*i)) < minDistance) {
                        minDistance = boost::geometry::comparable_distance(currentPoint, get(*i));
                        nearestPoint = i;
                    }
                }

                new_length += boost::geometry::distance(currentPoint, get(*(nearestPoint))); //Update the new path total length
                newpath.push_back(*(nearestPoint)); //Copy the chosen point into newpath
                currentPoint = get(*(nearestPoint));        //Set the next currentPoint to the chosen point
                temp_path.erase(nearestPoint);           //Remove the chosen point from the path list
            }

            newpath.push_back(temp_path.front());    //Copy the last point into newpath
            new_length += boost::geometry::distance(currentPoint, get(temp_path.front())); //Compute the distance and add it to new_length

            if (new_length < original_length)  //If the new path is better than the previous one
                path = newpath;
        }
    }
};

#endif
