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

#ifndef __ASP_TOOLS_BUNDLEADJUST_COST_FUNCTIONS_H__
#define __ASP_TOOLS_BUNDLEADJUST_COST_FUNCTIONS_H__

/**
  Ceres cost functions used by bundle_adjust.
*/
// TODO(oalexan1): Move most of this logic to the .cc file.

#include <vw/Camera/CameraUtilities.h>
#include <vw/Camera/OpticalBarModel.h>
#include <asp/Camera/CsmModel.h>
#include <asp/Camera/BundleAdjustCamera.h>
#include <asp/Core/StereoSettings.h>
#include <asp/Core/Macros.h>
#include <asp/Core/BundleAdjustUtils.h>

// Turn off warnings from eigen
#if defined(__GNUC__) || defined(__GNUG__)
#define LOCAL_GCC_VERSION (__GNUC__ * 10000                    \
                           + __GNUC_MINOR__ * 100              \
                           + __GNUC_PATCHLEVEL__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic push
#endif
#if LOCAL_GCC_VERSION >= 40202
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#endif

#include <ceres/ceres.h>
#include <ceres/loss_function.h>

#if defined(__GNUC__) || defined(__GNUG__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic pop
#endif
#undef LOCAL_GCC_VERSION
#endif

using namespace vw;
using namespace vw::camera;

typedef PixelMask<Vector<float, 2>> DispPixelT;

/// Used to accumulate the number of reprojection errors in bundle adjustment.
int g_ba_num_errors = 0;
Mutex g_ba_mutex;

double g_big_pixel_value = 1000.0;  // don't make this too big

//=====================================================================

/// Simple base class for unpacking Ceres parameter blocks into
///  a camera model which can do point projections.
class CeresBundleModelBase {
public:

  // These are the same for every camera.
  int num_point_params() const {return 3;}
  int num_pose_params () const {return 6;}

  /// This is for all camera parameters other than the pose parameters.
  /// - These can be spread out across multiple parameter blocks.
  virtual int num_intrinsic_params() const = 0;

  int num_params() const {
    return num_point_params() + num_pose_params() + num_intrinsic_params();
  }

  /// Return the number of Ceres input parameter blocks.
  virtual int num_parameter_blocks() const = 0;

  /// Return the size of each parameter block.
  /// - These should sum up to equal num_params.
  /// - The first block is always the point block (3) and
  ///   the second block is always the pose block (6).
  virtual std::vector<int> get_block_sizes() const {
    std::vector<int> result(2);
    result[0] = num_point_params();
    result[1] = num_pose_params();
    return result;
  }

  /// Read in all of the parameters and generate an output pixel observation.
  /// - Throws if the point does not project in to the camera.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const = 0;
  
}; // End class CeresBundleModelBase

/// Simple wrapper for the vw::camera::AdjustedCameraModel class with a 
/// preconfigured underlying camera.  Only uses translation and rotation.
/// - Just vary the six camera adjustment parameters which are all in 
///   a single parameter block.
class AdjustedCameraBundleModel: public CeresBundleModelBase {
public:

  AdjustedCameraBundleModel(boost::shared_ptr<vw::camera::CameraModel> cam) 
    : m_underlying_camera(cam) {}

  virtual int num_intrinsic_params() const {return 0;}

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point)
  virtual int num_parameter_blocks() const {return 2;}

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const {

    double const* raw_point = param_blocks[0];
    double const* raw_pose  = param_blocks[1];

    // Read the point location and camera information from the raw arrays.
    Vector3          point(raw_point[0], raw_point[1], raw_point[2]);
    CameraAdjustment correction(raw_pose);

    vw::camera::AdjustedCameraModel cam(m_underlying_camera,
					correction.position(),
					correction.pose());
    try {
      return cam.point_to_pixel(point);
    } catch(std::exception const& e){
      // This can be useful for debugging
      // std::cout << "Exception is " << e.what() << std::endl;
    }

    // We must not allow one bad point to ruin the optimization
    return vw::Vector2(g_big_pixel_value, g_big_pixel_value);
  }

private:

  /// This camera will be adjusted by the input parameters.
  boost::shared_ptr<vw::camera::CameraModel> m_underlying_camera;

}; // End class CeresBundleModelBase



/// "Full service" pinhole model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class PinholeBundleModel: public CeresBundleModelBase {
public:

  PinholeBundleModel(boost::shared_ptr<vw::camera::PinholeModel> cam)
    : m_underlying_camera(cam) {}

  /// The number of lens distortion parameters.
  int num_dist_params() const {
    vw::Vector<double> lens_params
      = m_underlying_camera->lens_distortion()->distortion_parameters();
    return lens_params.size();
  }

  virtual int num_intrinsic_params() const {
    // Center, focus, and lens distortion
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + num_dist_params(); 
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (lens distortion)
  virtual int num_parameter_blocks() const { return 5; }

  virtual std::vector<int> get_block_sizes() const {
    std::vector<int> result = CeresBundleModelBase::get_block_sizes();
    result.push_back(asp::NUM_CENTER_PARAMS);
    result.push_back(asp::NUM_FOCUS_PARAMS);
    result.push_back(num_dist_params());
    return result;
  }

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const {

    double const* raw_point  = param_blocks[0];
    double const* raw_pose   = param_blocks[1];
    double const* raw_center = param_blocks[2];
    double const* raw_focus  = param_blocks[3];
    double const* raw_lens   = param_blocks[4];

    // TODO: Should these values also be scaled?
    // Read the point location and camera information from the raw arrays.
    Vector3          point(raw_point[0], raw_point[1], raw_point[2]);
    CameraAdjustment correction(raw_pose);

    // We actually solve for scale factors for intrinsic values, so multiply them
    //  by the original intrinsic values to get the updated values.
    double center_x = raw_center[0] * m_underlying_camera->point_offset()[0];
    double center_y = raw_center[1] * m_underlying_camera->point_offset()[1];
    double focus    = raw_focus [0] * m_underlying_camera->focal_length()[0];

    // Update the lens distortion parameters in the new camera.
    // - These values are also optimized as scale factors.
    // TODO: This approach FAILS when the input value is zero!!
    boost::shared_ptr<LensDistortion> distortion 
      = m_underlying_camera->lens_distortion()->copy();
    vw::Vector<double> lens = distortion->distortion_parameters();
    for (size_t i=0; i<lens.size(); ++i)
      lens[i] *= raw_lens[i];
    distortion->set_distortion_parameters(lens);

    // Duplicate the input camera model with the pose, focus, center, and lens updated.
    // Respect m_u_direction, m_v_direction, m_w_direction in the original model.
    vw::camera::PinholeModel cam = *m_underlying_camera;
    cam.set_camera_center(correction.position());
    cam.set_camera_pose(correction.pose().rotation_matrix());
    cam.set_focal_length(vw::Vector2(focus, focus));
    cam.set_point_offset(vw::Vector2(center_x, center_y));
    cam.set_lens_distortion(distortion.get());
    cam.set_pixel_pitch(m_underlying_camera->pixel_pitch());
    try {
      // Project the point into the camera.
      Vector2 pixel = cam.point_to_pixel_no_check(point);
      return pixel;
    } catch(...) {
    }

    // Do not allow one bad pixel value to ruin the whole problem
    return vw::Vector2(g_big_pixel_value, g_big_pixel_value);
  }

private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<vw::camera::PinholeModel> m_underlying_camera;

}; // End class PinholeBundleModel

/// "Full service" optical bar model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class OpticalBarBundleModel: public CeresBundleModelBase {
public:

  OpticalBarBundleModel(boost::shared_ptr<vw::camera::OpticalBarModel> cam)
    : m_underlying_camera(cam) {}

  virtual int num_intrinsic_params() const {
    // Center, focus, and extra optical bar parameters
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + asp::NUM_OPTICAL_BAR_EXTRA_PARAMS;
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (other intrinsic parameters)
  virtual int num_parameter_blocks() const {return 5;}

  virtual std::vector<int> get_block_sizes() const {
    std::vector<int> result = CeresBundleModelBase::get_block_sizes();
    result.push_back(asp::NUM_CENTER_PARAMS);
    result.push_back(asp::NUM_FOCUS_PARAMS);
    result.push_back(asp::NUM_OPTICAL_BAR_EXTRA_PARAMS);
    return result;
  }

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const {

    double const* raw_point  = param_blocks[0];
    double const* raw_pose   = param_blocks[1];
    double const* raw_center = param_blocks[2];
    double const* raw_focus  = param_blocks[3];
    double const* raw_intrin = param_blocks[4];

    // TODO: Should these values also be scaled?
    // Read the point location and camera information from the raw arrays.
    Vector3          point(raw_point[0], raw_point[1], raw_point[2]);
    CameraAdjustment correction(raw_pose);

    // We actually solve for scale factors for intrinsic values, so multiply them
    //  by the original intrinsic values to get the updated values.
    double center_x  = raw_center[0] * m_underlying_camera->get_optical_center()[0];
    double center_y  = raw_center[1] * m_underlying_camera->get_optical_center()[1];
    double focus     = raw_focus [0] * m_underlying_camera->get_focal_length  ();
    double speed     = raw_intrin[0] * m_underlying_camera->get_speed();
    double mcf       = raw_intrin[1] * m_underlying_camera->get_motion_compensation();
    double scan_time = raw_intrin[2] * m_underlying_camera->get_scan_time();

    // Duplicate the input camera model with the pose, focus, center, speed, and MCF updated.
    vw::camera::OpticalBarModel cam(m_underlying_camera->get_image_size(),
                                     vw::Vector2(center_x, center_y),
                                     m_underlying_camera->get_pixel_size(),
                                     focus,
                                     scan_time,
                                     //m_underlying_camera->get_scan_rate(),
                                     m_underlying_camera->get_scan_dir(),
                                     m_underlying_camera->get_forward_tilt(),
                                     correction.position(),
                                     correction.pose().axis_angle(),
                                     speed,  mcf);

    // Project the point into the camera.
    try {
      return cam.point_to_pixel(point);
    } catch(std::exception const& e){
    }
    
    // We must not allow one bad point to ruin the optimization
    return vw::Vector2(g_big_pixel_value, g_big_pixel_value);
  }

private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<vw::camera::OpticalBarModel> m_underlying_camera;

}; // End class OpticalBarBundleModel

/// "Full service" CSM model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class CsmBundleModel: public CeresBundleModelBase {
public:

  CsmBundleModel(boost::shared_ptr<asp::CsmModel> cam):
   m_underlying_camera(cam) {}

  /// The number of lens distortion parameters.
  int num_dist_params() const {
    return m_underlying_camera->distortion().size();
  }

  virtual int num_intrinsic_params() const {
     // Center, focus, and lens distortion
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + num_dist_params();
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (lens distortion)
  virtual int num_parameter_blocks() const {return 5;}

  virtual std::vector<int> get_block_sizes() const {
    std::vector<int> result = CeresBundleModelBase::get_block_sizes();
    result.push_back(asp::NUM_CENTER_PARAMS);
    result.push_back(asp::NUM_FOCUS_PARAMS);
    result.push_back(num_dist_params());
    return result;
  }

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const {

    // TODO(oalexan1): Use here transformedCsmCamera() to avoid code repetition. 
    // But note that that one may set zero distortion to 1e-16 which likely here
    // we don't need to do.
    double const* raw_point  = param_blocks[0];
    double const* raw_pose   = param_blocks[1];
    double const* raw_center = param_blocks[2];
    double const* raw_focus  = param_blocks[3];
    double const* raw_dist   = param_blocks[4];

    // TODO: Should these values also be scaled?
    // Read the point location and camera information from the raw arrays.
    Vector3          point(raw_point[0], raw_point[1], raw_point[2]);
    CameraAdjustment correction(raw_pose);

    // We actually solve for scale factors for intrinsic values, so multiply them
    //  by the original intrinsic values to get the updated values.
    vw::Vector2 optical_center = m_underlying_camera->optical_center();
    double focal_length        = m_underlying_camera->focal_length();
    optical_center[0] = raw_center[0] * optical_center[0];
    optical_center[1] = raw_center[1] * optical_center[1];
    focal_length      = raw_focus [0] * focal_length;

    // Update the lens distortion parameters in the new camera.
    // - These values are also optimized as scale factors.
    std::vector<double> distortion = m_underlying_camera->distortion();
    for (size_t i = 0; i < distortion.size(); i++) {
      distortion[i] = raw_dist[i] * distortion[i];
    }

    // Duplicate the input camera model
    boost::shared_ptr<asp::CsmModel> copy;
    m_underlying_camera->deep_copy(copy);
    
    // Update the intrinsics of the copied model
    copy->set_optical_center(optical_center);
    copy->set_focal_length(focal_length);
    copy->set_distortion(distortion);

    // Form the adjusted camera. Note that unlike for Pinhole and Optical
    // bar, the parameters being optimized adjust the initial CSM camera,
    // rather than replacing it altogether. The CSM camera can in fact
    // be even linescan, when there would be many pose samples, in fact,
    // so it makes sense to work this way. 
    AdjustedCameraModel adj_cam(copy, correction.position(), correction.pose());

    try {
      // Project the point into the camera.
      Vector2 pixel = adj_cam.point_to_pixel(point);
      return pixel;
    } catch(...) {
    }

    // Do not allow one bad pixel value to ruin the whole problem
    return vw::Vector2(g_big_pixel_value, g_big_pixel_value);
  }

private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<asp::CsmModel> m_underlying_camera;

}; // End class CsmBundleModel


//=========================================================================
// Cost functions for Ceres

/// A Ceres cost function. We pass in the observation and the model.
///  The result is the residual, the difference in the observation 
///  and the projection of the point into the camera, normalized by pixel_sigma.
struct BaReprojectionError {
  BaReprojectionError(Vector2 const& observation, Vector2 const& pixel_sigma,
                      boost::shared_ptr<CeresBundleModelBase> camera_wrapper):
    m_observation(observation),
    m_pixel_sigma(pixel_sigma),
    m_num_param_blocks(camera_wrapper->num_parameter_blocks()),
    m_camera_wrapper(camera_wrapper)
    {}

  // Call to work with ceres::DynamicCostFunctions.
  // - Takes array of arrays.
  bool operator()(double const * const * parameters, double * residuals) const {

    try {
      // Unpack the parameter blocks
      std::vector<double const*> param_blocks(m_num_param_blocks);
      for (size_t i=0; i<m_num_param_blocks; ++i) {
        param_blocks[i] = parameters[i];
      }

      // Use the camera model wrapper to handle all of the parameter blocks.
      Vector2 prediction = m_camera_wrapper->evaluate(param_blocks);

      // The error is the difference between the predicted and observed pixel
      // position, normalized by sigma.
      residuals[0] = (prediction[0] - m_observation[0])/m_pixel_sigma[0];
      residuals[1] = (prediction[1] - m_observation[1])/m_pixel_sigma[1];

    } catch (std::exception const& e) { // TODO: Catch only projection errors?
      // Failed to compute residuals

      Mutex::Lock lock(g_ba_mutex);
      g_ba_num_errors++;
      if (g_ba_num_errors < 100) {
        vw_out(ErrorMessage) << e.what() << std::endl;
      }else if (g_ba_num_errors == 100) {
        vw_out() << "Will print no more error messages about "
                 << "failing to compute residuals.\n";
      }

      residuals[0] = g_big_pixel_value;
      residuals[1] = g_big_pixel_value;
      return false;
    }
    return true;
  }


  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(Vector2 const& observation,
                                     Vector2 const& pixel_sigma,
                                     boost::shared_ptr<CeresBundleModelBase> camera_wrapper){
    const int NUM_RESIDUALS = 2;

    ceres::DynamicNumericDiffCostFunction<BaReprojectionError>* cost_function =
        new ceres::DynamicNumericDiffCostFunction<BaReprojectionError>(
            new BaReprojectionError(observation, pixel_sigma, camera_wrapper));

    // The residual size is always the same.
    cost_function->SetNumResiduals(NUM_RESIDUALS);

    // The camera wrapper knows all of the block sizes to add.
    std::vector<int> block_sizes = camera_wrapper->get_block_sizes();
    for (size_t i=0; i<block_sizes.size(); ++i) {
      cost_function->AddParameterBlock(block_sizes[i]);
    }
    return cost_function;
  }

private:
  Vector2 m_observation;     ///< The pixel observation for this camera/point pair.
  Vector2 m_pixel_sigma;
  size_t  m_num_param_blocks;
  boost::shared_ptr<CeresBundleModelBase> m_camera_wrapper; ///< Pointer to the camera model object.

}; // End class BaReprojectionError

/// A ceres cost function. Here we float two pinhole camera's
/// intrinsic and extrinsic parameters. We take as input a reference
/// xyz point and a disparity from left to right image. The
/// error metric is the following: The reference xyz point is projected in the
/// left image. It is mapped via the disparity to the right
/// image. There, the residual error is the difference between that
/// pixel and the pixel obtained by projecting the xyz point
/// straight into the right image.
struct BaDispXyzError {
  BaDispXyzError(double max_disp_error,
                 double reference_terrain_weight,
                 Vector3 const& reference_xyz,
                 ImageViewRef<DispPixelT> const& interp_disp,
                 boost::shared_ptr<CeresBundleModelBase> left_camera_wrapper,
                 boost::shared_ptr<CeresBundleModelBase> right_camera_wrapper,
                 bool solve_intrinsics, // Would like to remove these!
                 asp::IntrinsicOptions intrinsics_opt):
  m_max_disp_error(max_disp_error),
  m_reference_terrain_weight(reference_terrain_weight),
  m_reference_xyz(reference_xyz),
  m_interp_disp (interp_disp),
  m_num_left_param_blocks (left_camera_wrapper->num_parameter_blocks ()),
  m_num_right_param_blocks(right_camera_wrapper->num_parameter_blocks()),
  m_left_camera_wrapper(left_camera_wrapper ),
  m_right_camera_wrapper(right_camera_wrapper),
  m_solve_intrinsics(solve_intrinsics),
  m_intrinsics_opt(intrinsics_opt) {}

  // Adaptor to work with ceres::DynamicCostFunctions.
  bool operator()(double const* const* parameters, double* residuals) const {

    try{
      // Split apart the input parameter blocks and hand them to the camera wrappers.
      std::vector<double const*> left_param_blocks, right_param_blocks;
      unpack_residual_pointers(parameters, left_param_blocks, right_param_blocks);

      // Get pixel projection in both cameras.
      Vector2 left_prediction  = m_left_camera_wrapper->evaluate (left_param_blocks );
      Vector2 right_prediction = m_right_camera_wrapper->evaluate(right_param_blocks);

      // See how consistent that is with the observed disparity.
      bool good_ans = true;
      if (!m_interp_disp.pixel_in_bounds(left_prediction)) {
        good_ans = false;
      }else{
        DispPixelT dispPix = m_interp_disp(left_prediction[0], left_prediction[1]);
        if (!is_valid(dispPix)) {
          good_ans = false;
        }else{
          Vector2 right_prediction_from_disp = left_prediction + dispPix.child();
          residuals[0] = right_prediction_from_disp[0] - right_prediction[0];
          residuals[1] = right_prediction_from_disp[1] - right_prediction[1];
          for (size_t it = 0; it < 2; it++) 
            residuals[it] *= m_reference_terrain_weight;
        }
      }

      // TODO: Think more of what to do below. The hope is that the robust cost
      // function will take care of big residuals graciously.
      if (!good_ans) {
        // Failed to find the residuals
        for (size_t it = 0; it < 2; it++) 
          residuals[it] = m_max_disp_error * m_reference_terrain_weight;
        return true;
      }

    } catch (const camera::PointToPixelErr& e) {
      // Failed to project into the camera
      for (size_t it = 0; it < 2; it++) 
        residuals[it] = m_max_disp_error * m_reference_terrain_weight;
      return true;
    }
    return true;
  }

  // TODO: Should this logic live somewhere else?
  /// Create the list of residual pointers when solving for intrinsics.
  /// - Extra logic is needed to avoid duplicate pointers.
  static void get_residual_pointers(asp::BAParams &param_storage,
                                    int left_cam_index, int right_cam_index,
                                    bool solve_intrinsics,
                                    asp::IntrinsicOptions const& intrinsics_opt,
                                    std::vector<double*> &residual_ptrs) {
    double* left_camera  = param_storage.get_camera_ptr(left_cam_index );
    double* right_camera = param_storage.get_camera_ptr(right_cam_index);
    residual_ptrs.clear();
    if (solve_intrinsics) {
      double* left_center      = param_storage.get_intrinsic_center_ptr    (left_cam_index );
      double* left_focus       = param_storage.get_intrinsic_focus_ptr     (left_cam_index );
      double* left_distortion  = param_storage.get_intrinsic_distortion_ptr(left_cam_index );
      double* right_center     = param_storage.get_intrinsic_center_ptr    (right_cam_index);
      double* right_focus      = param_storage.get_intrinsic_focus_ptr     (right_cam_index);
      double* right_distortion = param_storage.get_intrinsic_distortion_ptr(right_cam_index);

      residual_ptrs.push_back(left_camera    );
      residual_ptrs.push_back(left_center    );
      residual_ptrs.push_back(left_focus     );
      residual_ptrs.push_back(left_distortion);
      residual_ptrs.push_back(right_camera   );
      if (!intrinsics_opt.center_shared    ) residual_ptrs.push_back(right_center    );
      if (!intrinsics_opt.focus_shared     ) residual_ptrs.push_back(right_focus     );
      if (!intrinsics_opt.distortion_shared) residual_ptrs.push_back(right_distortion);
    }
    else { // This handles the generic camera case.
      residual_ptrs.push_back(left_camera );
      residual_ptrs.push_back(right_camera);
    }
    return;
  }
  
  void unpack_residual_pointers(double const* const* parameters,
                                std::vector<double const*> & left_param_blocks,
                                std::vector<double const*> & right_param_blocks) const {
    
    left_param_blocks.resize (m_num_left_param_blocks );
    right_param_blocks.resize(m_num_right_param_blocks);

    double const* raw_point = &(m_reference_xyz[0]);
    left_param_blocks [0] = raw_point; // The first input is always the point param block.
    right_param_blocks[0] = raw_point;

    int index = 0;
    for (size_t i=1; i<m_num_left_param_blocks; ++i) {
      left_param_blocks[i] = parameters[index];
      ++index;
    }
    if (!m_solve_intrinsics) {
      // Unpack everything from the right block in order.
      for (size_t i=1; i<m_num_right_param_blocks; ++i) {
        right_param_blocks[i] = parameters[index];
        ++index;
      }
    } else { // Solve for intrinsics. Handle shared intrinsics.
      right_param_blocks[1] = parameters[index]; // Pose and position
      ++index;
      if (m_intrinsics_opt.center_shared)
        right_param_blocks[2] = left_param_blocks[2];
      else {
        right_param_blocks[2] = parameters[index];
        ++index;
      }
      if (m_intrinsics_opt.focus_shared)
        right_param_blocks[3] = left_param_blocks[3];
      else {
        right_param_blocks[3] = parameters[index];
        ++index;
      }
      if (m_intrinsics_opt.distortion_shared)
        right_param_blocks[4] = left_param_blocks[4];
      else {
        right_param_blocks[4] = parameters[index];
        ++index;
      }
    } // End pinhole case
  }


  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(
      double max_disp_error, double reference_terrain_weight,
      Vector3 const& reference_xyz, ImageViewRef<DispPixelT> const& interp_disp,
      boost::shared_ptr<CeresBundleModelBase> left_camera_wrapper,
      boost::shared_ptr<CeresBundleModelBase> right_camera_wrapper,
      bool solve_intrinsics, asp::IntrinsicOptions intrinsics_opt = asp::IntrinsicOptions()) {

    const int NUM_RESIDUALS = 2;
    
    ceres::DynamicNumericDiffCostFunction<BaDispXyzError>* cost_function =
        new ceres::DynamicNumericDiffCostFunction<BaDispXyzError>(
            new BaDispXyzError(max_disp_error, reference_terrain_weight,
                               reference_xyz, interp_disp, 
                               left_camera_wrapper, right_camera_wrapper,
                               solve_intrinsics, intrinsics_opt));

    // The residual size is always the same.
    cost_function->SetNumResiduals(NUM_RESIDUALS);

    // Add all of the blocks for each camera, except for the first (point)
    // block which is provided at creation time.
    std::vector<int> block_sizes = left_camera_wrapper->get_block_sizes();
    for (size_t i=1; i<block_sizes.size(); ++i) {
      cost_function->AddParameterBlock(block_sizes[i]);
    }
    block_sizes = right_camera_wrapper->get_block_sizes();
    if (!solve_intrinsics) {
      for (size_t i=1; i<block_sizes.size(); ++i) {
        cost_function->AddParameterBlock(block_sizes[i]);
      }
    } else { // Pinhole handling
      if (block_sizes.size() != 5)
        vw_throw(LogicErr() << "Error: Pinhole camera model parameter number error!");
      cost_function->AddParameterBlock(block_sizes[1]); // The camera position/pose
      if (!intrinsics_opt.center_shared) cost_function->AddParameterBlock(block_sizes[2]);
      if (!intrinsics_opt.focus_shared) cost_function->AddParameterBlock(block_sizes[3]);
      if (!intrinsics_opt.distortion_shared) cost_function->AddParameterBlock(block_sizes[4]);
    }
    return cost_function;
  }  // End function Create

  double m_max_disp_error, m_reference_terrain_weight;
  Vector3 m_reference_xyz;
  ImageViewRef<DispPixelT> const& m_interp_disp;
  size_t m_num_left_param_blocks, m_num_right_param_blocks;
  // TODO: Make constant!
  boost::shared_ptr<CeresBundleModelBase> m_left_camera_wrapper;
  boost::shared_ptr<CeresBundleModelBase> m_right_camera_wrapper;

  // Would like to not have these two!
  bool m_solve_intrinsics;
  asp::IntrinsicOptions m_intrinsics_opt;
};


//===================================================================

/// A ceres cost function. The residual is the difference between the
/// observed 3D point and the current (floating) 3D point, normalized by
/// xyz_sigma. Used only for ground control points or with --tri-weight.
struct XYZError {
  XYZError(Vector3 const& observation, Vector3 const& xyz_sigma):
    m_observation(observation), m_xyz_sigma(xyz_sigma) {
      bool is_good = (xyz_sigma[0] > 0 && xyz_sigma[1] > 0 && xyz_sigma[2] > 0);
      if (!is_good) {
        // This will also cover NaNs
        vw_throw(ArgumentErr() << "XYZError: Invalid xyz_sigma: "
                 << xyz_sigma << ". All values must be positive.\n");
      }
    }

  template <typename T>
  bool operator()(const T* point, T* residuals) const {
    for (size_t p = 0; p < m_observation.size(); p++)
      residuals[p] = (point[p] - m_observation[p])/m_xyz_sigma[p]; // Units are meters

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(Vector3 const& observation,
                                     Vector3 const& xyz_sigma){
    return (new ceres::AutoDiffCostFunction<XYZError, 3, 3>
            (new XYZError(observation, xyz_sigma)));
  }

  Vector3 m_observation;
  Vector3 m_xyz_sigma;
};

/// This cost function imposes a rather hard constraint on camera center
/// horizontal and vertical motion. It does so by knowing how many reprojection
/// errors exist for this camera and making this cost function big enough to
/// overcome then when the motion is going out of bounds. The residual here is
/// raised to 4th power and will be squared when added to the cost function.
/// Two residuals are computed, for horizontal and vertical motion.
struct CamUncertaintyError {
  CamUncertaintyError(vw::Vector3 const& orig_ctr, double const* orig_adj,
                      vw::Vector2 const& uncertainty, int num_pixel_obs,
                      vw::cartography::Datum const& datum,
                      double camera_position_uncertainty_power):
    m_orig_ctr(orig_ctr), m_uncertainty(uncertainty), m_num_pixel_obs(num_pixel_obs),
    m_camera_position_uncertainty_power(camera_position_uncertainty_power) {
     
    // Ensure at least one term
    m_num_pixel_obs = std::max(m_num_pixel_obs, 1);
      
    // The first three parameters are the camera center adjustments.
    m_orig_adj = Vector3(orig_adj[0], orig_adj[1], orig_adj[2]);

    // The uncertainty must be positive
    if (m_uncertainty[0] <= 0 || m_uncertainty[1] <= 0)
      vw_throw(ArgumentErr() << "CamUncertaintyError: Invalid uncertainty: "
               << uncertainty << ". All values must be positive.\n");    
    
    // The NED coordinate system, for separating horizontal and vertical components
    vw::Vector3 llh = datum.cartesian_to_geodetic(orig_ctr);
    vw::Matrix3x3 NedToEcef = datum.lonlat_to_ned_matrix(llh);
    m_EcefToNed = vw::math::inverse(NedToEcef);
  }
    
  template <typename T>
  bool operator()(const T* cam_adj, T* residuals) const {
    
    // The difference between the original and current camera center
    vw::Vector3 diff;
    for (size_t p = 0; p < 3; p++)
      diff[p] = cam_adj[p] - m_orig_adj[p];
    
    // Convert the difference to NED
    vw::Vector3 NedDir = m_EcefToNed * diff;
    
    // Split into horizontal and vertical components
    vw::Vector2 horiz = subvector(NedDir, 0, 2);
    double      vert  = NedDir[2];
    
    // Normalize by uncertainty
    horiz /= m_uncertainty[0];
    vert  /= m_uncertainty[1];
    
    // In the final sum of squares, each term will end up being differences
    // raised to m_camera_position_uncertainty_power power.
    double p = m_camera_position_uncertainty_power / 4.0;
    residuals[0] = sqrt(m_num_pixel_obs) * pow(dot_prod(horiz, horiz), p);
    residuals[1] = sqrt(m_num_pixel_obs) * pow(vert * vert, p);

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(vw::Vector3 const& orig_ctr, double const* orig_adj,
                      vw::Vector2 const& uncertainty, int num_pixel_obs,
                      vw::cartography::Datum const& datum, 
                      double camera_position_uncertainty_power) {
    // 2 residuals and 3 translation variables. Must add the rotation variables, however,
    // for CERES not to complain. So, get 6.
    // ceres::RIDDERS works better than ceres::CENTRAL for this cost function,
    // especially when the uncertainty is 0.1 m or less.
    return (new ceres::NumericDiffCostFunction<CamUncertaintyError, ceres::RIDDERS, 2, 6>
            (new CamUncertaintyError(orig_ctr, orig_adj, uncertainty, num_pixel_obs, 
                                     datum, camera_position_uncertainty_power)));
  }

  // orig_ctr is the original camera center, orig_cam_ptr is the original
  // adjustment (resulting in the original center). The uncertainty is
  // in meters.
  vw::Vector3 m_orig_ctr;
  vw::Vector3 m_orig_adj;
  vw::Vector2 m_uncertainty;
  int m_num_pixel_obs;
  vw::Matrix3x3 m_EcefToNed;
  double m_camera_position_uncertainty_power;
};

/// A ceres cost function. The residual is the difference between the
/// observed 3D point lon-lat-height, and the current (floating) 3D
/// point lon-lat-height, normalized by sigma. Used only for
/// ground control points. This has the advantage, unlike
/// XYZError, that when the height is not known reliably,
/// but lon-lat is, we can, in the GCP file, assign a bigger
/// sigma to the latter.
struct LLHError {
  LLHError(Vector3 const& observation_xyz, Vector3 const& sigma, vw::cartography::Datum & datum):
    m_observation_xyz(observation_xyz), m_sigma(sigma), m_datum(datum){}

  template <typename T>
  bool operator()(const T* point, T* residuals) const {
    Vector3 observation_llh, point_xyz, point_llh;
    for (size_t p = 0; p < m_observation_xyz.size(); p++) {
      point_xyz[p] = double(point[p]);
    }

    point_llh       = m_datum.cartesian_to_geodetic(point_xyz);
    observation_llh = m_datum.cartesian_to_geodetic(m_observation_xyz);

    for (size_t p = 0; p < m_observation_xyz.size(); p++) 
      residuals[p] = (point_llh[p] - observation_llh[p])/m_sigma[p]; // Input units are meters

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(Vector3 const& observation_xyz,
                                     Vector3 const& sigma,
                                     vw::cartography::Datum & datum){

    return (new ceres::NumericDiffCostFunction<LLHError, ceres::CENTRAL, 3, 3>
            (new LLHError(observation_xyz, sigma, datum)));
  }

  Vector3 m_observation_xyz;
  Vector3 m_sigma;
  vw::cartography::Datum m_datum;
};


/// A ceres cost function. The residual is the difference between the
/// original camera center and the current (floating) camera center.
/// This cost function prevents the cameras from straying too far from
/// their starting point.
struct CamError {

  CamError(double const* orig_cam, double weight):
    m_orig_cam(DATA_SIZE), m_weight(weight){
      for (int i=0; i<DATA_SIZE; ++i)
        m_orig_cam[i] = orig_cam[i];
    }

  template <typename T>
  bool operator()(const T* cam_vec, T* residuals) const {

    const double POSITION_WEIGHT = 1e-2;  // Units are meters. Don't lock the camera down too tightly.
    const double ROTATION_WEIGHT = 5e1;   // Units are in radians. 

    for (size_t p = 0; p < DATA_SIZE/2; p++) {
      residuals[p] = POSITION_WEIGHT*m_weight*(cam_vec[p] - m_orig_cam[p]);
    }
    for (size_t p = DATA_SIZE/2; p < DATA_SIZE; p++) {
      residuals[p] = ROTATION_WEIGHT*m_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(const double *const orig_cam, double weight){
    return (new ceres::AutoDiffCostFunction<CamError, DATA_SIZE, DATA_SIZE>
            (new CamError(orig_cam, weight)));
  }

private:

  // The camera must be represented by a six element array.
  static const int DATA_SIZE = 6;

  std::vector<double> m_orig_cam;
  double m_weight;
};

/// A ceres cost function. The residual is the rotation + translation
/// vector difference, each multiplied by a weight. Hence, a larger
/// rotation weight will result in less rotation change in the final
/// result, etc. This is somewhat different than CamError as there is no
/// penalty here for this cost function going very large, the scaling is
/// different, and there is finer-grained control. 
struct RotTransError {

  RotTransError(double const* orig_cam, double rotation_weight, double translation_weight):
    m_orig_cam(DATA_SIZE), m_rotation_weight(rotation_weight),
    m_translation_weight(translation_weight) {
    for (int i=0; i<DATA_SIZE; ++i)
        m_orig_cam[i] = orig_cam[i];
    }

  template <typename T>
  bool operator()(const T* cam_vec, T* residuals) const {

    for (size_t p = 0; p < DATA_SIZE/2; p++) {
      residuals[p] = m_translation_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    for (size_t p = DATA_SIZE/2; p < DATA_SIZE; p++) {
      residuals[p] = m_rotation_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(const double *const orig_cam,
                                     double rotation_weight, double translation_weight){
    return (new ceres::AutoDiffCostFunction<RotTransError, DATA_SIZE, DATA_SIZE>
            (new RotTransError(orig_cam, rotation_weight, translation_weight)));

  }

private:

  // The camera must be represented by a six element array.
  static const int DATA_SIZE = 6;

  std::vector<double> m_orig_cam;
  double m_rotation_weight, m_translation_weight;
};

/// From the input options select the correct Ceres loss function.
ceres::LossFunction* get_loss_function(std::string const& cost_function, double th) {

  ceres::LossFunction* loss_function = NULL;
  if (cost_function == "l2")
    loss_function = NULL;
  else if (cost_function == "trivial")
    loss_function = new ceres::TrivialLoss();
  else if (cost_function == "huber")
    loss_function = new ceres::HuberLoss(th);
  else if (cost_function == "cauchy")
    loss_function = new ceres::CauchyLoss(th);
  else if (cost_function == "l1")
    loss_function = new ceres::SoftLOneLoss(th);
  else{
    vw::vw_throw(vw::ArgumentErr() << "Unknown cost function: " << cost_function << ".\n");
  }
  return loss_function;
}

#endif // __ASP_TOOLS_BUNDLEADJUST_COST_FUNCTIONS_H__

