/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#ifndef SPARSE_MAPPING_TENSOR_H_
#define SPARSE_MAPPING_TENSOR_H_

#include <Rig/RigCameraModel.h>
#include <Eigen/Geometry>
#include <ceres/ceres.h>

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <limits>
#include <memory>
#include <mutex>

namespace cv {
  class Mat;
  class DMatch;
}

namespace rig {
  struct nvmData;
  struct RigSet;
}

namespace sparse_mapping {

  class SparseMap;

  // Extract a submap in-place.
  void ExtractSubmap(std::vector<std::string> const& images_to_keep,
                     rig::nvmData & nvm);
  
  
}  // namespace sparse_mapping

#endif  // SPARSE_MAPPING_TENSOR_H_
