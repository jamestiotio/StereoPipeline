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

/// \file JitterSolveCostFuns.h

// Cost functions used in solving for jitter. These need access to the camera
// models, so they are stored in the Camera folder. The bigger functions defined
// here are implemented in the .cc file.

#ifndef __ASP_CAMERA_JITTER_SOLVE_COST_FUNS_H__
#define __ASP_CAMERA_JITTER_SOLVE_COST_FUNS_H__

#include <asp/Camera/CsmModel.h>
#include <asp/Camera/JitterSolveUtils.h>
#include <asp/Camera/JitterSolveRigUtils.h>
#include <asp/Camera/CsmUtils.h>
#include <asp/Core/BundleAdjustUtils.h>

#include <vw/Cartography/GeoReference.h>

#include <usgscsm/UsgsAstroLsSensorModel.h>
#include <usgscsm/UsgsAstroFrameSensorModel.h>
#include <usgscsm/Utilities.h>

#include <ceres/ceres.h>
#include <ceres/loss_function.h>

#include <string>
#include <map>
#include <vector>

namespace asp {

const double g_big_pixel_value = 1000.0;  // don't make this too big

struct BaBaseOptions;

// Calc the range of indices in the samples needed to interpolate between time1 and time2.
// Based on lagrangeInterp() in usgscsm.
void calcIndexBounds(double time1, double time2, double t0, double dt, int numVals,
                     // Outputs
                     int & begIndex, int & endIndex);

// Update the linescan model with the latest optimized values of the position
// and quaternion parameters. Also update the triangulated point.
void updateLsModelTriPt(double const * const * parameters, 
                        int begQuatIndex, int endQuatIndex,
                        int begPosIndex, int endPosIndex,
                        int & param_shift,
                        UsgsAstroLsSensorModel & cam,
                        csm::EcefCoord & P);

// Add the linescan model reprojection error to the cost function
void addLsReprojectionErr(asp::BaBaseOptions const & opt,
                          UsgsAstroLsSensorModel * ls_model,
                          vw::Vector2      const & observation,
                          double                 * tri_point,
                          double                   weight,
                          ceres::Problem         & problem);

// Add the frame camera model reprojection error to the cost function
void addFrameReprojectionErr(asp::BaBaseOptions  const & opt,
                             UsgsAstroFrameSensorModel * frame_model,
                             vw::Vector2         const & observation,
                             double                    * frame_params,
                             double                    * tri_point,
                             double                      weight,
                             ceres::Problem            & problem);

// Add reprojection errors. Collect data that will be used to add camera
// constraints that scale with the number of reprojection errors and GSD.
void addReprojCamErrs(asp::BaBaseOptions                const & opt,
                      asp::CRNJ                         const & crn,
                      std::vector<std::vector<vw::Vector2>> const & pixel_vec,
                      std::vector<std::vector<double>>  const & weight_vec,
                      std::vector<std::vector<int>>     const & isAnchor_vec,
                      std::vector<std::vector<int>>     const & pix2xyz_index,
                      std::vector<asp::CsmModel*>       const & csm_models,
                      bool                                      have_rig,
                      rig::RigSet                        const& rig,
                      std::vector<RigCamInfo>            const& rig_cam_info,
                      // Outputs
                      std::vector<double>                     & tri_points_vec,
                      std::vector<double>                     & frame_params,
                      std::vector<double>                     & weight_per_residual,
                      std::vector<std::vector<double>>        & weight_per_cam,
                      std::vector<std::vector<double>>        & count_per_cam,
                      std::vector<double>                     & ref_to_curr_sensor_vec,
                      ceres::Problem                          & problem);

// Add the constraint based on DEM
void addDemConstraint(asp::BaBaseOptions       const& opt,
                      std::vector<vw::Vector3> const& dem_xyz_vec,
                      std::set<int>            const& outliers,
                      vw::ba::ControlNetwork   const& cnet,
                      // Outputs
                      std::vector<double>           & tri_points_vec,
                      std::vector<double>           & weight_per_residual, // append
                      ceres::Problem                & problem);

// Add the constraint to keep triangulated points close to initial values
// This does not need a DEM or alignment
void addTriConstraint(asp::BaBaseOptions     const& opt,
                      std::set<int>          const& outliers,
                      vw::ba::ControlNetwork const& cnet,
                      asp::CRNJ              const& crn,
                      // Outputs
                      std::vector<double>    & tri_points_vec,
                      std::vector<double>    & weight_per_residual, // append
                      ceres::Problem         & problem);

// Add camera constraints that are proportional to the number of reprojection errors.
// This requires going through some of the same motions as in addReprojCamErrs().
void addCamPositionConstraint(asp::BaBaseOptions               const& opt,
                              std::set<int>                    const& outliers,
                              asp::CRNJ                        const& crn,
                              std::vector<asp::CsmModel*>      const& csm_models,
                              std::vector<std::vector<double>> const& weight_per_cam,
                              std::vector<std::vector<double>> const& count_per_cam,
                              bool                                    have_rig,
                              rig::RigSet                      const& rig,
                              std::vector<asp::RigCamInfo>     const& rig_cam_info,
                              // Outputs
                              std::vector<double>                & frame_params,
                              std::vector<double>                & weight_per_residual, 
                              ceres::Problem                     & problem);

void addQuatNormRotationConstraints(
    asp::BaBaseOptions            const& opt,
    std::set<int>                 const& outliers,
    asp::CRNJ                     const& crn,
    std::vector<asp::CsmModel*>   const& csm_models,
    bool                                 have_rig,
    rig::RigSet                   const& rig,
    std::vector<RigCamInfo>       const& rig_cam_info,
    double                               quat_norm_weight, 
    // Outputs
    std::vector<double>                & frame_params,
    std::vector<double>                & weight_per_residual, // append
    ceres::Problem                     & problem);

// Add roll / yaw constraints. For linescan, use the whole set of samples for given
// camera model. For frame cameras, use the trajectory of all cameras in the same orbital
// group as the current camera.
void addRollYawConstraint(asp::BaBaseOptions              const& opt,
                          asp::CRNJ                       const& crn,
                          std::vector<asp::CsmModel*>     const& csm_models,
                          vw::cartography::GeoReference   const& georef,
                          std::map<int, int>              const& orbital_groups,
                          bool initial_camera_constraint,
                          double roll_weight, double yaw_weight,
                          // Outputs (append to residual)
                          std::vector<double>                  & frame_params,
                          std::vector<double>                  & weight_per_residual,
                          ceres::Problem                       & problem);

} // end namespace asp

#endif //__ASP_CAMERA_JITTER_SOLVE_COST_FUNS_H__
