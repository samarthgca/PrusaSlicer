/*================================================================*/
/*
 * Author:  Pavel Surynek, 2023 - 2024
 * Company: Prusa Research
 *
 * File:    seq_utilities.cpp
 *
 * Various utilities for sequential print.
 */
/*================================================================*/

#include <sstream>
#include <iostream>
#include <fstream>

#include "seq_defs.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include "seq_utilities.hpp"


/*----------------------------------------------------------------*/

using namespace std;
using namespace Slic3r;


/*----------------------------------------------------------------*/

namespace Sequential
{
     

/*----------------------------------------------------------------*/


bool find_and_remove(std::string& src, const std::string& key)
{
    size_t pos = src.find(key);
    if (pos != std::string::npos) {
        src.erase(pos, key.length());
        return true;
    }
    return false;
}


std::vector<ObjectToPrint> load_exported_data(const std::string& filename)
{
    std::vector<ObjectToPrint> objects_to_print;

    std::ifstream in(filename);
    if (!in)
        throw std::runtime_error("NO EXPORTED FILE WAS FOUND");
    std::string line;

    while (in) {        
        std::getline(in, line);
        if (find_and_remove(line, "OBJECT_ID")) {
            objects_to_print.push_back(ObjectToPrint());
            objects_to_print.back().id = std::stoi(line);
        }
        if (find_and_remove(line, "TOTAL_HEIGHT"))
	{
            objects_to_print.back().total_height = std::stoi(line);
	}
        if (find_and_remove(line, "POLYGON_AT_HEIGHT"))
	{
            objects_to_print.back().pgns_at_height.emplace_back(std::make_pair(std::stoi(line), Polygon()));
	}
        if (find_and_remove(line, "POINT"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            Point pt(std::stoi(val), 0);
            ss >> val;
            pt.y() = std::stoi(val);
            objects_to_print.back().pgns_at_height.back().second.append(pt);
        }
    }
    return objects_to_print;
}


int load_printer_geometry(const std::string& filename, PrinterGeometry &printer_geometry)
{
    std::ifstream in(filename);
    if (!in)
        throw std::runtime_error("NO PRINTER GEOMETRY FILE WAS FOUND");
    std::string line;

    Polygon *current_polygon = NULL;
    
    while (in) {        
        std::getline(in, line);
	
	if (find_and_remove(line, "POLYGON_AT_HEIGHT"))
	{
	    coord_t height = std::stoi(line);

	    std::map<coord_t, std::vector<Polygon> >::iterator extruder_slice = printer_geometry.extruder_slices.find(height);
		
	    if (extruder_slice != printer_geometry.extruder_slices.end())
	    {
		extruder_slice->second.push_back(Polygon());
		current_polygon = &extruder_slice->second.back();
	    }
	    else
	    {
		vector<Polygon> polygons;
		polygons.push_back(Polygon());
		    
		current_polygon = &printer_geometry.extruder_slices.insert(std::pair(height, polygons)).first->second.back();
	    }
	}
        else if (find_and_remove(line, "POINT"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            Point pt(std::stoi(val), 0);
            ss >> val;
            pt.y() = std::stoi(val);

	    assert(current_polygon != NULL);
            current_polygon->append(pt);
        }
	else if (find_and_remove(line, "CONVEX_HEIGHT"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            coord_t height = std::stoi(val);

	    printer_geometry.convex_heights.insert(height);
	}
	else if (find_and_remove(line, "BOX_HEIGHT"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            coord_t height = std::stoi(val);
	    
	    printer_geometry.box_heights.insert(height);
	}

	else if (find_and_remove(line, "X_SIZE"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            coord_t x_size = std::stoi(val);
	    
	    printer_geometry.x_size = x_size;
	}
	else if (find_and_remove(line, "Y_SIZE"))
	{
            std::stringstream ss(line);
            std::string val;
            ss >> val;
            coord_t y_size = std::stoi(val);
	    
	    printer_geometry.y_size = y_size;
	}			
    }
    return 0;    
}

    
void save_import_data(const std::string           &filename,
		      const std::map<double, int> &scheduled_polygons,
		      const map<int, int>         &original_index_map,
		      const vector<Rational>      &poly_positions_X,
		      const vector<Rational>      &poly_positions_Y)
{
    std::ofstream out(filename);
    if (!out)
        throw std::runtime_error("CANNOT CREATE IMPORT FILE");

    for (const auto& scheduled_polygon: scheduled_polygons)
    {
	coord_t X, Y;

	scaleUp_PositionForSlicer(poly_positions_X[scheduled_polygon.second],
				  poly_positions_Y[scheduled_polygon.second],
				  X,
				  Y);
	const auto& original_index = original_index_map.find(scheduled_polygon.second);
	    
//	out << original_index_map[scheduled_polygon.second] << " " << X << " " << Y << endl;
	out << original_index->second << " " << X << " " << Y << endl;	    
    }
}


/*----------------------------------------------------------------*/

} // namespace Sequential
