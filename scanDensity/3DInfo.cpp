/**
  Implements the CloudAnalyzer3D which is responsible for analyzing
  3D point and free space evidence.  Due to the increased complexity
  of this data, it is also responsible for saving it
*/

#include "scanDensity_3DInfo.h"

voxel::CloudAnalyzer3D::CloudAnalyzer3D(
    const std::shared_ptr<const std::vector<Eigen::Vector3f>> &points,
    const std::shared_ptr<const std::vector<Eigen::Matrix3d>> &R,
    const std::shared_ptr<const BoundingBox> &bBox)
    : points{points}, R{R}, bBox{bBox} {}

void voxel::CloudAnalyzer3D::run(double voxelsPerMeter, double pixelsPerMeter) {
  bBox->getBoundingBox(pointMin, pointMax);
  this->voxelsPerMeter = voxelsPerMeter;
  this->pixelsPerMeter = pixelsPerMeter;

  const int numX = voxelsPerMeter * (pointMax[0] - pointMin[0]);
  const int numY = voxelsPerMeter * (pointMax[1] - pointMin[1]);

  const float zScale = voxelsPerMeter;
  const int numZ = zScale * (pointMax[2] - pointMin[2]);

  pointsPerVoxel.assign(numZ, Eigen::MatrixXi::Zero(numY, numX));

  for (auto &point : *points) {
    const int x = voxelsPerMeter * (point[0] - pointMin[0]);
    const int y = voxelsPerMeter * (point[1] - pointMin[1]);
    const int z = zScale * (point[2] - pointMin[2]);

    if (x < 0 || x >= numX)
      continue;
    if (y < 0 || y >= numY)
      continue;
    if (z < 0 || z >= numZ)
      continue;

    ++pointsPerVoxel[z](y, x);
  }

  // Free space evidence

  float cameraCenter[3];
  cameraCenter[0] = -1 * pointMin[0];
  cameraCenter[1] = -1 * pointMin[1];
  cameraCenter[2] = -1 * pointMin[2];
  numTimesSeen.assign(numZ, Eigen::MatrixXi::Zero(numY, numX));

  for (int k = 0; k < numZ; ++k) {
    for (int i = 0; i < numX; ++i) {
      for (int j = 0; j < numY; ++j) {
        if (!pointsPerVoxel[k](j, i))
          continue;

        Eigen::Vector3d ray;
        ray[0] = i - cameraCenter[0] * voxelsPerMeter;
        ray[1] = j - cameraCenter[1] * voxelsPerMeter;
        ray[2] = k - cameraCenter[2] * zScale;
        double length = ray.norm();
        Eigen::Vector3d unitRay = ray / length;

        int stop = floor(0.85 * length - 3);
        Eigen::Vector3i voxelHit;
        for (int a = 0; a < stop; ++a) {
          voxelHit[0] =
              floor(cameraCenter[0] * voxelsPerMeter + a * unitRay[0]);
          voxelHit[1] =
              floor(cameraCenter[1] * voxelsPerMeter + a * unitRay[1]);
          voxelHit[2] = floor(cameraCenter[2] * zScale + a * unitRay[2]);

          if (voxelHit[0] < 0 || voxelHit[0] >= numX)
            continue;
          if (voxelHit[1] < 0 || voxelHit[1] >= numY)
            continue;
          if (voxelHit[2] < 0 || voxelHit[2] >= numZ)
            continue;

          numTimesSeen[voxelHit[2]](voxelHit[1], voxelHit[0]) +=
              pointsPerVoxel[k](j, i);
        }
      }
    }
  }

  zeroZeroD =
      Eigen::Vector3d(-pointMin[0] * voxelsPerMeter,
                      -pointMin[1] * voxelsPerMeter, -pointMin[2] * zScale);
  zeroZero =
      Eigen::Vector3i(-pointMin[0] * voxelsPerMeter,
                      -pointMin[1] * voxelsPerMeter, -pointMin[2] * zScale);
}
template <typename T> static void displayVoxelGrid(const T &voxelB) {
  Eigen::MatrixXd collapsed =
      Eigen::MatrixXd::Zero(voxelB.v[0].rows(), voxelB.v[0].cols());

  for (int k = 0; k < voxelB.v.size(); ++k)
    for (int i = 0; i < voxelB.v[0].cols(); ++i)
      for (int j = 0; j < voxelB.v[0].rows(); ++j)
        collapsed(j, i) += voxelB.v[k](j, i) ? 1 : 0;

  double average, sigma;
  average = sigma = 0;
  int count = 0;
  const double *dataPtr = collapsed.data();
  for (int i = 0; i < collapsed.size(); ++i) {
    if (*(dataPtr + i)) {
      ++count;
      average += *(dataPtr + i);
    }
  }

  average = average / count;

  for (int i = 0; i < collapsed.size(); ++i)
    if (*(dataPtr + i) != 0)
      sigma += (*(dataPtr + i) - average) * (*(dataPtr + i) - average);

  sigma = sigma / (count - 1);
  sigma = sqrt(sigma);

  cv::Mat heatMap(collapsed.rows(), collapsed.cols(), CV_8UC3,
                  cv::Scalar::all(255));
  for (int i = 0; i < heatMap.rows; ++i) {
    uchar *dst = heatMap.ptr<uchar>(i);
    for (int j = 0; j < heatMap.cols; ++j) {
      if (collapsed(i, j)) {
        const int gray = cv::saturate_cast<uchar>(
            255.0 * (collapsed(i, j) - average) / (1.0 * sigma));
        int red, green, blue;
        if (gray < 128) {
          red = 0;
          green = 2 * gray;
          blue = 255 - green;
        } else {
          blue = 0;
          red = 2 * (gray - 128);
          green = 255 - red;
        }
        dst[j * 3] = blue;
        dst[j * 3 + 1] = green;
        dst[j * 3 + 2] = red;
      }
    }
  }
  cvNamedWindow("Preview", CV_WINDOW_NORMAL);
  cv::imshow("Preview", heatMap);
  cv::waitKey(0);
}

/* This method is destructive to the things created by run */
void voxel::CloudAnalyzer3D::saveVoxelGrids(
    const std::vector<fs::path> &pointNames,
    const std::vector<fs::path> &freeNames, const fs::path &metaData) {
  std::vector<Eigen::MatrixXi> &pointGrid = pointsPerVoxel;
  std::vector<Eigen::MatrixXi> &freeSpace = numTimesSeen;

  const int z = pointGrid.size();
  const int y = pointGrid[0].rows();
  const int x = pointGrid[0].cols();

  double averageP = 0.0, averageF = 0.0;
  int countP = 0, countF = 0;

  for (int i = 0; i < z; ++i) {
    const auto *dataPtr = pointGrid[i].data();
    const auto *fPtr = freeSpace[i].data();
    for (int j = 0; j < pointGrid[i].size(); ++j) {
      const int value = *(dataPtr + j);
      if (value) {
        averageP += value;
        ++countP;
      }
      if (*(fPtr + j)) {
        averageF += *(fPtr + j);
        ++countF;
      }
    }
  }
  averageP /= countP;
  averageF /= countF;
  double sigmaP = 0.0, sigmaF = 0.0;
  for (int i = 0; i < z; ++i) {
    const auto *dataPtr = pointGrid[i].data();
    const auto *fPtr = freeSpace[i].data();
    for (int j = 0; j < pointGrid[i].size(); ++j) {
      const int value = *(dataPtr + j);
      if (value)
        sigmaP += (value - averageP) * (value - averageP);
      if (*(fPtr + j))
        sigmaF += (*(fPtr + j) - averageF) * (*(fPtr + j) - averageF);
    }
  }
  sigmaP /= countP - 1;
  sigmaP = sqrt(sigmaP);
  sigmaF /= countF - 1;
  sigmaF = sqrt(sigmaF);

  std::vector<Eigen::MatrixXb> threshHoldedPoint(z,
                                                 Eigen::MatrixXb::Zero(y, x));
  std::vector<Eigen::MatrixXb> threshHoldedFree(z, Eigen::MatrixXb::Zero(y, x));
  size_t numNonZeros = 0, nonZeroPoint = 0;
  for (int k = 0; k < z; ++k) {
    const auto *pointSrc = pointGrid[k].data();
    auto *pointDst = threshHoldedPoint[k].data();

    const auto *freeSrc = freeSpace[k].data();
    char *freeDst = threshHoldedFree[k].data();
    for (int i = 0; i < x * y; ++i) {
      if (*(pointSrc + i)) {
        double normalized = (*(pointSrc + i) - averageP) / sigmaP;
        *(pointDst + i) = normalized > -1.0 ? 1 : 0;
        nonZeroPoint += normalized > -1.0 ? 1 : 0;
      }

      if (*(freeSrc + i)) {
        double normalized = (*(freeSrc + i) - averageP) / sigmaP;
        *(freeDst + i) = normalized > -1.0 ? 1 : 0;
        numNonZeros += normalized > -1.0 ? 1 : 0;
      }
    }
  }
  pointGrid.clear();
  freeSpace.clear();

  int newRows = sqrt(2) * std::max(y, x);
  int newCols = newRows;
  int dX = (newCols - x) / 2.0;
  int dY = (newRows - y) / 2.0;
  Eigen::Vector3d newZZ = zeroZeroD;
  newZZ[0] += dX;
  newZZ[1] += dY;

  std::ofstream metaDataWriter(metaData.string(),
                               std::ios::out | std::ios::binary);
  for (int r = 0; r < NUM_ROTS; ++r) {
    place::VoxelGrid rotatedFree, rotatedPoint;
    rotatedFree.v = std::vector<Eigen::MatrixXb>(
        z, Eigen::MatrixXb::Zero(newRows, newCols));
    rotatedFree.c = numNonZeros;

    rotatedPoint.v = std::vector<Eigen::MatrixXb>(
        z, Eigen::MatrixXb::Zero(newRows, newCols));
    rotatedPoint.c = nonZeroPoint;
    for (int k = 0; k < z; ++k) {
      for (int i = 0; i < newCols; ++i) {
        for (int j = 0; j < newRows; ++j) {
          Eigen::Vector3d point(i, j, k);
          Eigen::Vector3d src = R->at(r) * (point - newZZ) + zeroZeroD;

          if (src[0] < 0 || src[0] >= x)
            continue;
          if (src[1] < 0 || src[1] >= y)
            continue;
          if (src[2] < 0 || src[2] >= z)
            continue;

          rotatedFree.v[k](j, i) = threshHoldedFree[src[2]](src[1], src[0]);
          rotatedPoint.v[k](j, i) = threshHoldedPoint[src[2]](src[1], src[0]);
        }
      }
    }

    int minCol = newCols;
    int minRow = newRows;
    int maxCol = 0;
    int maxRow = 0;
    int minZ = z;
    int maxZ = 0;
    for (int k = 0; k < z; ++k) {
      for (int i = 0; i < newCols; ++i) {
        for (int j = 0; j < newRows; ++j) {
          if (rotatedPoint.v[k](j, i)) {
            minCol = std::min(minCol, i);
            maxCol = std::max(maxCol, i);

            minRow = std::min(minRow, j);
            maxRow = std::max(maxRow, j);

            minZ = std::min(minZ, k);
            maxZ = std::max(maxZ, k);
          }
        }
      }
    }

    const int newZ = maxZ - minZ + 1;
    const int newY = maxRow - minRow + 1;
    const int newX = maxCol - minCol + 1;

    place::VoxelGrid trimmedFree, trimmedPoint;
    trimmedFree.v = std::vector<Eigen::MatrixXb>(newZ);
    trimmedFree.c = rotatedFree.c;

    trimmedPoint.v = std::vector<Eigen::MatrixXb>(newZ);
    trimmedPoint.c = rotatedPoint.c;

    for (int k = 0; k < newZ; ++k) {
      trimmedFree.v[k] =
          rotatedFree.v[k + minZ].block(minRow, minCol, newY, newX);
      trimmedPoint.v[k] =
          rotatedPoint.v[k + minZ].block(minRow, minCol, newY, newX);
    }
    rotatedFree.v.clear();
    rotatedPoint.v.clear();

    if (FLAGS_visulization) {
      displayVoxelGrid(trimmedPoint);
      displayVoxelGrid(trimmedFree);
    }

    place::MetaData meta{zeroZero, newX,           newY,
                         newZ,     voxelsPerMeter, pixelsPerMeter};
    meta.zZ[0] += dX;
    meta.zZ[1] += dY;
    meta.zZ[0] -= minCol;
    meta.zZ[1] -= minRow;
    meta.zZ[2] -= minZ;
    meta.writeToFile(metaDataWriter);

    trimmedPoint.zZ = meta.zZ;
    trimmedFree.zZ = meta.zZ;

    std::ofstream out(freeNames[r].string(), std::ios::out | std::ios::binary);
    trimmedFree.writeToFile(out);
    out.close();

    out.open(pointNames[r].string(), std::ios::out | std::ios::binary);
    trimmedPoint.writeToFile(out);
    out.close();
  }
  metaDataWriter.close();
}