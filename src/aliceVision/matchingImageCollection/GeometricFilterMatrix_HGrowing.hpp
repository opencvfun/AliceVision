// This file is part of the AliceVision project.
// Copyright (c) 2016 AliceVision contributors.
// Copyright (c) 2012 openMVG contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "aliceVision/multiview/homographyKernelSolver.hpp"
//#include "aliceVision/multiview/affineSolver.hpp"
#include "aliceVision/robustEstimation/ACRansac.hpp"
#include "aliceVision/robustEstimation/ACRansacKernelAdaptator.hpp"
//#include "aliceVision/robustEstimation/guidedMatching.hpp"

#include "aliceVision/matching/IndMatch.hpp"
//#include "aliceVision/matching/IndMatchDecorator.hpp"
#include "aliceVision/sfm/SfMData.hpp"
#include "aliceVision/feature/RegionsPerView.hpp"
#include "aliceVision/matchingImageCollection/GeometricFilterMatrix.hpp"
#include "Eigen/Geometry"


namespace aliceVision {
namespace matchingImageCollection {

//-- Multiple homography matrices estimation template functor, based on homography growing, used for filter pair of putative correspondences
struct GeometricFilterMatrix_HGrowing : public GeometricFilterMatrix
{
  GeometricFilterMatrix_HGrowing(
    double dPrecision = std::numeric_limits<double>::infinity(),
    size_t iteration = 1024)
    : GeometricFilterMatrix(dPrecision, std::numeric_limits<double>::infinity(), iteration)
    , _maxNbHomographies(10)
    , _minNbMatchesPerH(20)
    , _similarityTolerance(10)
    , _affinityTolerance(10)
    , _homographyTolerance(5)
    , _minInliersToRefine(6)
    , _nbIterations(8)
    , _maxFractionPlanarMatches(0.7)
  {
    _Hs.push_back(Mat3::Identity());
  }
/**
   * @brief Given two sets of image points, it estimates the homography matrix
   * relating them using a robust method (like A Contrario Ransac).
   */
  template<typename Regions_or_Features_ProviderT>
  EstimationStatus geometricEstimation(
    const sfm::SfMData * sfmData,
    const Regions_or_Features_ProviderT& regionsPerView,
    const Pair pairIndex,
    const matching::MatchesPerDescType & putativeMatchesPerType,
    matching::MatchesPerDescType & out_geometricInliersPerType)
  {
    // EOrdering - Ouput ordering : 
    // * Putative: 'out_geometricInliersPerType' contains matches with the same ordering than 'putativeMatchesPerType'
    // * Homographies: matches sharing the same homography are assembled. 
    enum EOrdering {PutativeLike, HGrouped}; 
    EOrdering orderingMethod = HGrouped;
     
    using namespace aliceVision;
    using namespace aliceVision::robustEstimation;
    out_geometricInliersPerType.clear();
    
    // Get back corresponding view index
    const IndexT viewId_I = pairIndex.first;
    const IndexT viewId_J = pairIndex.second;
    
    if (viewId_I == 200563944 && viewId_J == 1112206013) // [TEMP] MATLAB exemple
    {
      const std::vector<feature::EImageDescriberType> descTypes = regionsPerView.getCommonDescTypes(pairIndex);
      if(descTypes.empty())
        return EstimationStatus(false, false);
      
      const feature::Regions & regionsSIFT_I = regionsPerView.getRegions(viewId_I, descTypes.at(0));
      const feature::Regions & regionsSIFT_J = regionsPerView.getRegions(viewId_J, descTypes.at(0));
      const std::vector<feature::SIOPointFeature> allSIFTFeaturesI = getSIOPointFeatures(regionsSIFT_I);
      const std::vector<feature::SIOPointFeature> allSIFTfeaturesJ = getSIOPointFeatures(regionsSIFT_J);
      
      const std::size_t nbPutativeMatches = putativeMatchesPerType.at(feature::EImageDescriberType::SIFT).size();
      
      matching::IndMatches remainingSIFTMatches = putativeMatchesPerType.at(feature::EImageDescriberType::SIFT);
                  
      for (IndexT iH = 0; iH < _maxNbHomographies; ++iH)
      {
        ALICEVISION_LOG_DEBUG("Computing homography no. " << iH << "...");
        
        std::set<IndexT> visitedMatchesId, bestMatchesId;
        Mat3 bestHomographie;
        
#pragma omp parallel for
        for (IndexT iMatch = 0; iMatch < remainingSIFTMatches.size(); ++iMatch)
        {
          // [1st improvement ([F.Srajer, 2016] p. 20) ] Each match is used once only per homography estimation (increases computation time)
          if (visitedMatchesId.find(iMatch) != visitedMatchesId.end()) 
            continue;
          
          // Growing a homography from one match ([F.Srajer, 2016] algo. 1, p. 20)  
          std::set<IndexT> planarMatchesId; // be careful: it contains the id. in the 'remainingMatches' vector not 'putativeMatches' vector.
          Mat3 homographie;
          
          if(!growHomography(allSIFTFeaturesI, allSIFTfeaturesJ, remainingSIFTMatches, iMatch, planarMatchesId, homographie) == EXIT_SUCCESS)
            continue;
          
#pragma omp critical
          visitedMatchesId.insert(planarMatchesId.begin(), planarMatchesId.end());
          
          if (planarMatchesId.size() > bestMatchesId.size())
          {
#pragma omp critical
            {
              bestMatchesId = planarMatchesId;
              bestHomographie = homographie;
            }
          }
        } // IndexT iMatch
        
        // Stop when the models get to small        
        if (bestMatchesId.size() < _minNbMatchesPerH)
        {
          ALICEVISION_LOG_DEBUG("Stop: Planar models get to small: " << bestMatchesId.size() << "/" <<  _minNbMatchesPerH);
          break;
        }
        
        // { ...  remaining
        // [TODO] 3rd improvement: non lin optimization
        // ... }
        
        if (orderingMethod == EOrdering::HGrouped)
        {
          for (IndexT id : bestMatchesId)
          {
            out_geometricInliersPerType[feature::EImageDescriberType::SIFT].push_back(remainingSIFTMatches.at(id));
          }
        }    
        
        // Update remaining matches (/!\ Keep ordering):
        std::size_t cpt = 0;
        for (IndexT id : bestMatchesId) 
        {        
          remainingSIFTMatches.erase(remainingSIFTMatches.begin() + id - cpt);
          ++cpt;
        }
        
        ALICEVISION_LOG_DEBUG("\t- best H found: [ " 
                              << bestHomographie(0,0) << " " << bestHomographie(0,1) << " " << bestHomographie(0,2) << " ; " 
                              << bestHomographie(1,0) << " " << bestHomographie(1,1) << " " << bestHomographie(1,2) << " ; " 
                              << bestHomographie(2,0) << " " << bestHomographie(2,1) << " " << bestHomographie(2,2) << " ]"); 
        ALICEVISION_LOG_DEBUG("\t- " << bestMatchesId.size() << " corresponding planar matches.");
        ALICEVISION_LOG_TRACE("\t- " << remainingSIFTMatches.size() << " remaining matches.");
        
        // Stop when the number of remaining matches is too small   
        if (remainingSIFTMatches.size() < _minNbMatchesPerH)
        {
          ALICEVISION_LOG_TRACE("Stop: Not enought remaining matches (: " << remainingSIFTMatches.size() << "/" << _minNbMatchesPerH << " min.)");
          break;
        }
      } // IndexT iH
      
            
      // Copy inliers -> putative matches ordering
      if (orderingMethod == EOrdering::PutativeLike)
      {
        out_geometricInliersPerType[feature::EImageDescriberType::SIFT] =  putativeMatchesPerType.at(feature::EImageDescriberType::SIFT);
        matching::IndMatches & outSiftMatches = out_geometricInliersPerType.at(feature::EImageDescriberType::SIFT);
        for (IndexT iMatch = 0; iMatch < outSiftMatches.size(); ++iMatch)
        {
          const matching::IndMatch & match = outSiftMatches.at(iMatch);
          std::vector<matching::IndMatch>::iterator it = std::find(remainingSIFTMatches.begin(), 
                                                                   remainingSIFTMatches.end(), 
                                                                   match);
          if (it != remainingSIFTMatches.end()) // is not a verified match
          {
            outSiftMatches.erase(outSiftMatches.begin() + iMatch);
            remainingSIFTMatches.erase(it); // to decrease complexity (does not used anymore)
            --iMatch;
          }
        }
      }
    }
    
    // Check if resection has strong support
    const bool hasStrongSupport = true;
    return EstimationStatus(true, hasStrongSupport);
  }
    
  /**
   * @brief Geometry_guided_matching
   * @param sfm_data
   * @param regionsPerView
   * @param pairIndex
   * @param dDistanceRatio
   * @param matches
   * @return
   */
  bool Geometry_guided_matching
  (
    const sfm::SfMData * sfmData,
    const feature::RegionsPerView & regionsPerView,
    const Pair imageIdsPair,
    const double dDistanceRatio,
    matching::MatchesPerDescType & matches) override
  {
    
    /* ... */
    return matches.getNbAllMatches() != 0;
  }

private:

  // Growing a homography from one match ([F.Srajer, 2016] algo. 1, p. 20)  
  //-- See: YASM/relative_pose.h
  int growHomography(const std::vector<feature::SIOPointFeature> & featuresI, 
                      const std::vector<feature::SIOPointFeature> & featuresJ, 
                      const matching::IndMatches & matches,
                      const IndexT & seedMatchId,
                      std::set<IndexT> & planarMatchesIndices, 
                      Mat3 & transformation)
  {
    ALICEVISION_LOG_TRACE("Growing homography:"
                          "\n- #matches = " << matches.size() <<
                          "\n- seed match id. = " << seedMatchId);
                          
    assert(seedMatchId <= matches.size());
   
    planarMatchesIndices.clear();
    transformation = Mat3::Identity();
    
    const matching::IndMatch & seedMatch = matches.at(seedMatchId);
    const feature::SIOPointFeature & seedFeatureI = featuresI.at(seedMatch._i);
    const feature::SIOPointFeature & seedFeatureJ = featuresJ.at(seedMatch._j);

    std::size_t currTolerance;

    for (IndexT iRefineStep = 0; iRefineStep < _nbIterations; ++iRefineStep)
    {
      if (iRefineStep == 0)
      {
        ALICEVISION_LOG_TRACE("Step " << iRefineStep << "/" << _nbIterations << ": Similarity");
          
        computeSimilarity(seedFeatureI, seedFeatureJ, transformation);
        currTolerance = _similarityTolerance;

        ALICEVISION_LOG_TRACE("|- T_similarity = \n" << transformation);
      }
      else if (iRefineStep <= 4)
      {
        ALICEVISION_LOG_TRACE("Step " << iRefineStep << "/" << _nbIterations << ": Affinity");

        estimateAffinity(featuresI, featuresJ, matches, transformation, planarMatchesIndices);
        currTolerance = _affinityTolerance;

        ALICEVISION_LOG_TRACE("|- T_affinity= \n" << transformation);
      }
      else
      {
        ALICEVISION_LOG_TRACE("Step " << iRefineStep << "/" << _nbIterations << ": Homography");
                
        estimateHomography(featuresI, featuresJ, matches, transformation, planarMatchesIndices);
        currTolerance = _homographyTolerance;

        ALICEVISION_LOG_TRACE("|- T_homography =  \n" << transformation);
      }
      
      findTransformationInliers(featuresI, featuresJ, matches, transformation, currTolerance, planarMatchesIndices);
      
      ALICEVISION_LOG_TRACE("|- " << planarMatchesIndices.size() << " matches corresponding to the transformation." );
      
      if (planarMatchesIndices.size() < _minInliersToRefine)
      {
        ALICEVISION_LOG_TRACE("Stop: Not enought planar matches to estimate H (: " << planarMatchesIndices.size() << "/" << _minInliersToRefine << ").");
        return EXIT_FAILURE;
      }
      
//      // Note: the following statement is present in the MATLAB code but not implemented in YASM
//      if (planarMatchesIndices.size() >= _maxFractionPlanarMatches * matches.size())
//        break;
    }
    
    return (transformation != Mat3::Identity()) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  
  /**
   * @brief estimateHomography
   * see: by DLT: alicevision::homography::kernel::FourPointSolver::Solve() [multiview/homographyKernelSolver.hpp]
   *      by RANSAC: alicevision::matchingImageCOllection::geometricEstimation() [matchingImageCollection/GeometricFilterMatrix_H_AC.hpp]
   *      [git] https://github.com/fsrajer/yasfm/blob/master/YASFM/relative_pose.cpp#L1694
   */
  void estimateHomography(const std::vector<feature::SIOPointFeature> & featuresI,
                          const std::vector<feature::SIOPointFeature> & featuresJ,
                          const matching::IndMatches & matches,
                          Mat3 &H,
                          const std::set<IndexT> & usefulMatchesId = std::set<IndexT>())
  {
    assert(!featuresI.empty());
    assert(!featuresJ.empty());
    assert(!matches.empty());
    assert(*std::max_element(usefulMatchesId.begin(), usefulMatchesId.end()) <= matches.size()); // prevent segfault
    
    H = Mat3::Identity();
    
    std::size_t nbMatches = usefulMatchesId.size();
    
    std::set<IndexT> matchesId = usefulMatchesId; // duplicate

    if (usefulMatchesId.empty())
    {
      nbMatches = matches.size();
      // set every match as useful for estimation
      for (IndexT i = 0; i < nbMatches; ++i)
        matchesId.insert(i);
    }
    
    Mat3 CI, CJ;
    centeringMatrices(featuresI, featuresJ, matches, CI, CJ, matchesId);
        
    Mat A(Mat::Zero(2*nbMatches,9));
    
    IndexT iMatch = 0;
    for(IndexT matchId : matchesId)
    {
      const feature::SIOPointFeature & featI = featuresI.at(matches.at(matchId)._i);
      const feature::SIOPointFeature & featJ = featuresJ.at(matches.at(matchId)._j);
      Vec2 fI(featI.x(), featI.y()); 
      Vec2 fJ(featJ.x(), featJ.y());
      Vec3 ptI = CI * fI.homogeneous();
      Vec3 ptJ = CJ * fJ.homogeneous();
      
      A.block(iMatch,0,1,3) = ptI.transpose();
      A.block(iMatch,6,1,3) = -ptJ(0) * ptI.transpose();
      A.block(iMatch+nbMatches,3,1,3) = ptI.transpose();
      A.block(iMatch+nbMatches,6,1,3) = -ptJ(1) * ptI.transpose();
      ++iMatch;
    }
    
    Eigen::JacobiSVD<Mat> svd(A, Eigen::ComputeThinU | Eigen::ComputeFullV);
    Vec h = svd.matrixV().rightCols(1);
    Mat3 H0;
    H0.row(0) = h.topRows(3).transpose();
    H0.row(1) = h.middleRows(3,3).transpose();
    H0.row(2) = h.bottomRows(3).transpose();
    
    H = CJ.inverse() * H0 * CI;
    H /= H(2,2);
  }
    
  /**
   * @brief estimateAffinity
   * see: 
   * https://eigen.tuxfamily.org/dox/group__LeastSquares.html
   * [ppt] https://www.google.fr/url?sa=t&rct=j&q=&esrc=s&source=web&cd=5&ved=0ahUKEwjg4JrL66PaAhWOyKQKHRz_BJ0QFghKMAQ&url=https%3A%2F%2Fcourses.cs.washington.edu%2Fcourses%2Fcse576%2F02au%2Flectures%2FMatching2D.ppt&usg=AOvVaw3dEP3al4Y-27r6e9FMYGrz
   * [git] https://github.com/fsrajer/yasfm/blob/master/YASFM/relative_pose.cpp#L1669
   */
  void estimateAffinity(const std::vector<feature::SIOPointFeature> & featuresI,
                        const std::vector<feature::SIOPointFeature> & featuresJ,
                        const matching::IndMatches & matches,
                        Mat3 & affineTransformation,
                        const std::set<IndexT> & usefulMatchesId = std::set<IndexT>())
  {
    assert(!featuresI.empty());
    assert(!featuresJ.empty());
    assert(!matches.empty());
    assert(*std::max_element(usefulMatchesId.begin(), usefulMatchesId.end()) <= matches.size()); // prevent segfault

    affineTransformation = Mat3::Identity();
    
    std::set<IndexT> matchesId = usefulMatchesId; // duplicate
    
    std::size_t nbMatches = usefulMatchesId.size();
    
    if (usefulMatchesId.empty())
    {
      nbMatches = matches.size();
      // set every match as useful for estimation
      for (IndexT i = 0; i < nbMatches; ++i)
        matchesId.insert(i);
    }
    
    Mat M(Mat::Zero(2*nbMatches,6));
    Vec b(2*nbMatches);
    int iMatch = 0;
    for (IndexT matchId : matchesId)
    {
      const feature::SIOPointFeature & featI = featuresI.at(matches.at(matchId)._i);
      const feature::SIOPointFeature & featJ = featuresJ.at(matches.at(matchId)._j);
      Vec2 featICoords (featI.x(), featI.y());

      M.block(iMatch,0,1,3) = featICoords.homogeneous().transpose();
      M.block(iMatch+nbMatches,3,1,3) = featICoords.homogeneous().transpose();
      b(iMatch) = featJ.x();
      b(iMatch+nbMatches) = featJ.y();

      ++iMatch;
    }
    
    Vec a = M.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);
    affineTransformation.row(0) = a.topRows(3).transpose();
    affineTransformation.row(1) = a.bottomRows(3).transpose();
    affineTransformation(2,0) = 0.;
    affineTransformation(2,1) = 0.;
    affineTransformation(2,2) = 1.;
  }

  /**
   * @brief computeSimilarityFromMatch
   * see: alicevision::sfm::computeSimilarity() [sfm/utils/alignment.cpp]
   *      alicevision::geometry::ACRansac_FindRTS() [geometry/rigidTransformation3D(_test).hpp]
   */   
  void computeSimilarity(const feature::SIOPointFeature & feat1,
                         const feature::SIOPointFeature & feat2,
                         Mat3 & S)
  {
    S = Mat3::Identity(); 
        
    const Vec2f & coord1 = feat1.coords();
    const double & scale1 = feat1.scale();
    const double & orientation1 = feat1.orientation();
    
    const Vec2f & coord2 = feat2.coords();
    const double & scale2 =  feat2.scale();
    const double & orientation2 = feat2.orientation();              
    
    double c1 = cos(orientation1),
        s1 = sin(orientation1),
        c2 = cos(orientation2),
        s2 = sin(orientation2);
    
    Mat3 A1,A2;
    A1 << scale1*c1,scale1*(-s1),coord1(0),
        scale1*s1,scale1*c1,coord1(1),
        0,0,1;
    A2 << scale2*c2,scale2*(-s2),coord2(0),
        scale2*s2,scale2*c2,coord2(1),
        0,0,1;
    
    S = A2*A1.inverse();                               
  }
  
  /**
   * @brief findHomographyInliers Test the reprojection error
   */
  void findTransformationInliers(const std::vector<feature::SIOPointFeature> & featuresI, 
                                 const std::vector<feature::SIOPointFeature> & featuresJ, 
                                 const matching::IndMatches & matches,
                                 const Mat3 & transformation,
                                 const std::size_t tolerance,
                                 std::set<IndexT> & planarMatchesIndices)
  {
    planarMatchesIndices.clear();
  
#pragma omp parallel for 
    for (IndexT iMatch = 0; iMatch < matches.size(); ++iMatch)
    {
      const feature::SIOPointFeature & featI = featuresI.at(matches.at(iMatch)._i);
      const feature::SIOPointFeature & featJ = featuresJ.at(matches.at(iMatch)._j);
      
      Vec2 ptI(featI.x(), featI.y());
      Vec2 ptJ(featJ.x(), featJ.y());
      
      Vec3 ptIp_hom = transformation * ptI.homogeneous();

      float dist = (ptJ - ptIp_hom.hnormalized()).squaredNorm();   
      
      if (dist < Square(tolerance))
      {
#pragma omp critical
        planarMatchesIndices.insert(iMatch);
      }
    }
  }

  void centeringMatrices(const std::vector<feature::SIOPointFeature> & featuresI,
                         const std::vector<feature::SIOPointFeature> & featuresJ,
                         const matching::IndMatches & matches,
                         Mat3 & cI,
                         Mat3 & cJ,
                         const std::set<IndexT> & usefulMatchesId = std::set<IndexT>())
  {
    assert(!featuresI.empty());
    assert(!featuresJ.empty());
    assert(!matches.empty());
    assert(*std::max_element(usefulMatchesId.begin(), usefulMatchesId.end()) <= matches.size()); // prevent segfault
    
    std::set<IndexT> matchesId = usefulMatchesId; // duplicate
    std::size_t nbMatches = usefulMatchesId.size();
    
    if (usefulMatchesId.empty())
    {
      nbMatches = matches.size();
      // set every match as useful for estimation
      for (IndexT i = 0; i < nbMatches; ++i)
        matchesId.insert(i);
    }
    
    int iMatch = 0;
    Matf ptsI(2, nbMatches);
    Matf ptsJ(2, nbMatches);
    
    for (IndexT matchId : matchesId)
    {
      ptsI.col(iMatch) = featuresI.at(matches.at(matchId)._i).coords();
      ptsJ.col(iMatch) = featuresJ.at(matches.at(matchId)._j).coords();
      ++iMatch;
    }
    
    centerMatrix(ptsI, cI);
    centerMatrix(ptsJ, cJ);
  }
  
  void centerMatrix(const Eigen::Matrix2Xf & points2d, Mat3 & c)
  {
      c = Mat3::Identity();
      
      Vec2f mean = points2d.rowwise().mean();
      std::size_t nbPoints = points2d.cols();
      
      Vec2f stdDev = ((points2d.colwise() - mean).cwiseAbs2().rowwise().sum()/(nbPoints - 1)).cwiseSqrt();
      
      if(stdDev(0) < 0.1)
        stdDev(0) = 0.1;
      if(stdDev(1) < 0.1)
        stdDev(1) = 0.1;
      
      c << 1./stdDev(0), 0.,            -mean(0)/stdDev(0),
          0.,            1./stdDev(1),  -mean(1)/stdDev(1),
          0.,            0.,            1.;
  }
 
  //-- Stored data
  std::vector<Mat3> _Hs;
  
  //-- Options
  
  std::size_t _maxNbHomographies; // = MaxHoms
  std::size_t _minNbMatchesPerH; // = MinInsNum
  
  // growHomography function:
  std::size_t _similarityTolerance; // = SimTol
  std::size_t _affinityTolerance;   // = AffTol
  std::size_t _homographyTolerance; // = HomTol
  
  std::size_t _minInliersToRefine; // = MinIns
  std::size_t _nbIterations; // = RefIterNum
  std::size_t _maxFractionPlanarMatches; // = StopInsFrac
  
  
}; // struct GeometricFilterMatrix_HGrowing

} // namespace matchingImageCollection
} // namespace aliceVision

