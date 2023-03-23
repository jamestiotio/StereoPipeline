// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

/// \file SfsImageProc.cc
/// Image processing routines for SfS

#include <asp/Core/SfsImageProc.h>

#include <vw/Core/Log.h>
#include <vw/Camera/CameraModel.h>
#include <vw/Cartography/GeoReference.h>
#include <vw/FileIO/DiskImageView.h>
#include <vw/FileIO/GdalWriteOptions.h>
#include <vw/Cartography/GeoReferenceUtils.h>
#include <asp/Core/BundleAdjustUtils.h>

#include <boost/filesystem.hpp>

#include <string>

namespace fs = boost::filesystem;
using namespace vw;


namespace asp {

// Given a set of images of same dimensions, find the per-pixel maximum.
void maxImage(int cols, int rows,
              std::set<int> const& skip_images,
              std::vector<vw::ImageView<double>> const& images,
              ImageView<double> & max_image) {
  
  int num_images = images.size();

  max_image.set_size(cols, rows);
  for (int col = 0; col < cols; col++) {
    for (int row = 0; row < rows; row++) {
      max_image(col, row) = 0.0;
    }
  }

  for (int image_iter = 0; image_iter < num_images; image_iter++) {

    if (skip_images.find(image_iter) != skip_images.end())
      continue;

    auto & img = images[image_iter]; // alias
    if (img.cols() <= 0 || img.rows() <= 0) 
      continue;
    
    if (img.cols() != cols || img.rows() != rows) 
      vw::vw_throw(vw::ArgumentErr() << "The input DEM and computed extended images "
                   << "must have the same dimensions.\n");
    
    for (int col = 0; col < img.cols(); col++) {
      for (int row = 0; row < img.rows(); row++) {
        max_image(col, row) = std::max(max_image(col, row), img(col, row));
      }
    }
  }

  return;
}

// Given an image with float pixels, find the pixels where the image
// value is non-positive but some of its neighbors have positive
// values. Create an image which has the value 1 at such pixels and
// whose values linearly decrease to 0 both in the direction of pixels
// with positive and non-positive input values. 
void boundaryWeight(int blending_dist, ImageView<double> const & image, // inputs
                    ImageView<double> & boundary_weight) { // output
  
  double blending_dist_sq = blending_dist * blending_dist;
  int max_dist_int = ceil(blending_dist); // an int overestimate

  // Initialize the output to 0
  int cols = image.cols(), rows = image.rows();
  boundary_weight.set_size(cols, rows);
  for (int col = 0; col < cols; col++) {
    for (int row = 0; row < rows; row++) {
      boundary_weight(col, row) = 0.0;
    }
  }

  for (int col = 0; col < cols; col++) {
    for (int row = 0; row < rows; row++) {
          
      // Look for a boundary pixel, which is a pixel with non-positive
      // value but with neighbors with positive value
      if (image(col, row) > 0)
        continue;
      
      bool is_bd_pix = false;
      for (int c = col - 1; c <= col + 1; c++) {
        for (int r = row - 1; r <= row + 1; r++) {
          if (c < 0 || c >= cols || r < 0 || r >= rows)
            continue;
          if (image(c, r) > 0) {
            is_bd_pix = true;
            break; // found it
          }
        }
        if (is_bd_pix) 
          break; // found it
      }
      if (!is_bd_pix) 
        continue; // did not find it
          
      // Found the boundary pixel. Increase the weight in the circular
      // neighborhood. It will decay to 0 at the boundary of this
      // neighborhood.
      for (int c = col - max_dist_int; c <= col + max_dist_int; c++) {
        for (int r = row - max_dist_int; r <= row + max_dist_int; r++) {
          if (c < 0 || c >= cols || r < 0 || r >= rows)
            continue;
              
          // Cast to double before multiplying to avoid integer overflow
          double dsq = double(c - col) * double(c - col) + 
            double(r - row) * double(r - row);
              
          // Too far 
          if (dsq >= blending_dist_sq) 
            continue;
              
          double d = sqrt(dsq);
          d = blending_dist - d; // get a cone pointing up, with base at height 0.
          d /= double(blending_dist); // make it between 0 and 1
          d = std::max(d, 0.0); // should not be necessary
          // Add its contribution
          boundary_weight(c, r) = std::max(boundary_weight(c, r), d);
        }
      }
    }
  }

  return;
}

// Given an image with non-negative values, create another image
// which is 1 where the input image has positive values, and decays
// to 0 linearly beyond that.
void extendedWeight(int blending_dist, ImageView<double> const & image, // inputs         
                    ImageView<double> & extended_weight) { // output

  int cols = image.cols(), rows = image.rows();
  extended_weight.set_size(cols, rows);
  for (int col = 0; col < image.cols(); col++) {
    for (int row = 0; row < image.rows(); row++) {
      extended_weight(col, row) = (image(col, row) > 0);
    }
  }
  
  double blending_dist_sq = blending_dist * blending_dist;
  int max_dist_int = ceil(blending_dist); // an int overestimate
  
  for (int col = 0; col < cols; col++) {
    for (int row = 0; row < rows; row++) {
      
      // Look for a boundary pixel, which is a pixel with zero
          // weight but with neighbors with positive weight
      if (image(col, row) > 0)
            continue;
      bool is_bd_pix = false;
      for (int c = col - 1; c <= col + 1; c++) {
        for (int r = row - 1; r <= row + 1; r++) {
          if (c < 0 || c >= cols || r < 0 || r >= rows)
            continue;
          if (image(c, r) > 0) {
            is_bd_pix = true;
            break; // found it
          }
        }
        if (is_bd_pix) 
          break; // found it
      }
      if (!is_bd_pix) 
        continue; // did not find it
      
      // Found the boundary pixel. Increase the weight in the
      // circular neighborhood.  It will still be below 1
      // and decay to 0 at the boundary of this neighborhood.
      for (int c = col - max_dist_int; c <= col + max_dist_int; c++) {
        for (int r = row - max_dist_int; r <= row + max_dist_int; r++) {
          if (c < 0 || c >= cols || r < 0 || r >= rows)
            continue;
          
          // Cast to double before multiplying to avoid integer overflow
          double dsq = double(c - col) * double(c - col) + 
            double(r - row) * double(r - row);
          
          // Too far 
          if (dsq >= blending_dist_sq) 
            continue;
          
          double d = sqrt(dsq);
          d = blending_dist - d; // get a cone pointing up, with base at height 0.
          d /= double(blending_dist); // make it between 0 and 1
          d = std::max(d, 0.0); // should not be necessary
          // Add its contribution
          extended_weight(c, r) = std::max(extended_weight(c, r), d);
        }
      }
    }
  }

  return;
}
  
// Find the function which is 1 on the boundary of the max lit region
// and linearly decays to 0 away from it. Add portions of this to the
// image blending weights, in proportion to how relevant the images are
// likely to contribute. Hence, in the area where all data is
// borderline, we give more weight to the borderline data, because
// there is nothing else.  This improves the reconstruction.
// Note: Input image blending weights are 1 away from shadows and decay to 0
// at the shadow boundary. Output weights will decay then to 0 a bit
// deeper in the shadow area where there is no other data. We do not
// recompute these weights as the DEM changes, which is an approximation.
void adjustBorderlineDataWeights(int cols, int rows,
                                 int blending_dist, double blending_power,
                                 vw::GdalWriteOptions const& opt,
                                 vw::cartography::GeoReference const& geo,
                                 std::set<int> const& skip_images,
                                 std::string const& out_prefix, // for debug data
                                 std::vector<std::string> const& input_images, 
                                 std::vector<std::string> const& input_cameras, 
                                 std::vector<vw::ImageView<double>> & ground_weights) {
  
  int num_images = ground_weights.size();

  // Find the max per-pixel weight
  ImageView<double> max_weight;
  maxImage(cols, rows, skip_images, ground_weights,
           max_weight);

  // Find a weight which is 1 at the max-lit/unlit interface and decaying linearly
  // to 0.
  ImageView<double> boundary_weight;
  boundaryWeight(blending_dist, max_weight, // inputs
                 boundary_weight); // output

  // For an input ground weight (which shows where the image is lit),
  // find the the weight which is 1 inside where
  // the image pixels are lit, and linearly decreases from 1 to 0 at
  // image boundary (outwardly, in the area of unlit pixels).
  std::vector<ImageView<double>> extended_weights(num_images);
  for (int image_iter = 0; image_iter < num_images; image_iter++) {

    if (skip_images.find(image_iter) != skip_images.end())
      continue;
    auto & ground_wt = ground_weights[image_iter]; // alias
    if (ground_wt.cols() <= 0 || ground_wt.rows() <= 0) 
      continue;
    if (ground_wt.cols() != cols || ground_wt.rows() != rows) 
      vw::vw_throw(vw::ArgumentErr() << "The input DEM and computed extended "
                   << "weights must have the same dimensions.\n");
    
    extendedWeight(blending_dist, ground_wt,  // inputs         
                   extended_weights[image_iter]); // output
  } // end iterating over images

  // Distribute the boundary weight to each extended image weight
  // intersecting with it. Then add that contribution to the existing
  // image weight.
  for (int col = 0; col < cols; col++) {
    for (int row = 0; row < rows; row++) {
          
      // Find the sum at each pixel
      double sum = 0.0;
      for (int image_iter = 0; image_iter < num_images; image_iter++) {
        if (skip_images.find(image_iter) != skip_images.end())
          continue;
        auto & extended_wt = extended_weights[image_iter]; // alias
        if (extended_wt.cols() > 0 && extended_wt.rows() > 0
            && extended_wt(col, row) > 0) {
          sum += extended_wt(col, row);
        }
      }
      if (sum <= 0) 
        continue;
          
      for (int image_iter = 0; image_iter < num_images; image_iter++) {
        
        if (skip_images.find(image_iter) != skip_images.end())
          continue;
        
        auto & extended_wt = extended_weights[image_iter]; // alias
        if (extended_wt.cols() <= 0 || extended_wt.rows() <= 0 ||
            extended_wt(col, row) <= 0) continue;
        
        // This is the core of the logic. When only for one image this
        // pixel is lit, ensure this weight is 1. When there's a lot
        // of them, ensure the others don't dilute this weight. But
        // still ensure this weight is continuous.
        double delta_wt = extended_wt(col, row) * std::max(1.0, 1.0/sum); 
        
        // Restrict this to the max-lit mosaic boundary
        delta_wt *= boundary_weight(col, row);
        
        // Undo the power in the weight being passed in, add the new
        // contribution, extending it, and put back the power.
        double ground_wt = ground_weights[image_iter](col, row);
        ground_wt = pow(ground_wt, 1.0/blending_power);
        ground_wt = ground_wt + delta_wt;
        ground_wt = pow(ground_wt, blending_power);
        ground_weights[image_iter](col, row) = ground_wt;
      }
    }
  }

  extended_weights.clear(); // not needed anymore

  // TODO(oalexan1): Must make sure to make the images have non-negative but valid values
  // where the weights are positive and invalid values where they are zero.

  bool save_debug_info = false;
  if (save_debug_info) {
    bool has_georef = true, has_nodata = false;
    float img_nodata_val = false; // will not be used
    std::string max_weight_file = out_prefix + "-max_weight.tif";
    vw_out() << "Writing: " << max_weight_file << std::endl;
    vw::cartography::block_write_gdal_image(max_weight_file,
                                            max_weight,
                                            has_georef, geo, has_nodata,
                                            img_nodata_val, opt,
                                            TerminalProgressCallback("asp", ": "));
      
    std::string boundary_weight_file = out_prefix + "-boundary_weight.tif";
    vw_out() << "Writing: " << boundary_weight_file << std::endl;
    vw::cartography::block_write_gdal_image(boundary_weight_file,
                                            boundary_weight,
                                            has_georef, geo, has_nodata,
                                            img_nodata_val, opt,
                                            TerminalProgressCallback("asp", ": "));
      
    for (int image_iter = 0; image_iter < num_images; image_iter++) {

      if (skip_images.find(image_iter) != skip_images.end())
        continue;

      std::string out_camera_file
        = asp::bundle_adjust_file_name(out_prefix,
                                       input_images[image_iter],
                                       input_cameras[image_iter]);
      std::string local_prefix = fs::path(out_camera_file).replace_extension("").string();
    
      bool has_georef = true, has_nodata = false;
      std::string ground_weight_file = local_prefix + "-ground_weight.tif";
      vw_out() << "Writing: " << ground_weight_file << std::endl;
      vw::cartography::block_write_gdal_image(ground_weight_file,
                                              ground_weights[image_iter],
                                              has_georef, geo, has_nodata,
                                              img_nodata_val, opt,
                                              TerminalProgressCallback("asp", ": "));
        
    }
  } // end saving debug info

  return;
}
  
} // end namespace asp
