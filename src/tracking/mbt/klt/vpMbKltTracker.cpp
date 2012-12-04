/****************************************************************************
 *
 * $Id$
 *
 * Copyright (C) 1998-2010 Inria. All rights reserved.
 *
 * This software was developed at:
 * IRISA/INRIA Rennes
 * Projet Lagadic
 * Campus Universitaire de Beaulieu
 * 35042 Rennes Cedex
 * http://www.irisa.fr/lagadic
 *
 * This file is part of the ViSP toolkit
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined by Trolltech AS of Norway and appearing in the file
 * LICENSE included in the packaging of this file.
 *
 * Licensees holding valid ViSP Professional Edition licenses may
 * use this file in accordance with the ViSP Commercial License
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Contact visp@irisa.fr if any conditions of this licensing are
 * not clear to you.
 *
 * Description:
 * Model based tracker using only KLT
 *
 * Authors:
 * Romain Tallonneau
 * Aurelien Yol
 *
 *****************************************************************************/

#include <visp/vpMbKltTracker.h>

#ifdef VISP_HAVE_OPENCV

vpMbKltTracker::vpMbKltTracker()
{
  cur = NULL;
  compute_interaction = true;
  firstInitialisation = true;

  tracker.setTrackerId(1);
  tracker.setUseHarris(1);
  
  tracker.setMaxFeatures(10000);
  tracker.setWindowSize(5);
  tracker.setQuality(0.01);
  tracker.setMinDistance(5);
  tracker.setHarrisFreeParameter(0.01);
  tracker.setBlockSize(3);
  tracker.setPyramidLevels(3);
  
  angleAppears = vpMath::rad(90);
  angleDisappears = vpMath::rad(90);
  
  maskBorder = 10;
  threshold_outlier = 0.5;
 
  lambda = 0.8;
  maxIter = 200;
}

/*!
  Basic destructor.

*/
vpMbKltTracker::~vpMbKltTracker()
{
  if(cur != NULL){
    cvReleaseImage(&cur);
    cur = NULL;
  }
}

void 
vpMbKltTracker::init(const vpImage<unsigned char>& _I)
{
  if(!modelInitialised){
    throw vpException(vpException::fatalError, "model not initialised");
  }
  if(!cameraInitialised){
    throw vpException(vpException::fatalError, "camera not initialised");
  }
  this->c0Mo = cMo;
  ctTc0.setIdentity();

  vpImageConvert::convert(_I,cur);
  
  // mask
  IplImage* mask = cvCreateImage(cvSize(_I.getWidth(), _I.getHeight()), IPL_DEPTH_8U, 1);
  cvZero(mask);
  
  for (unsigned int i = 0; i < faces.size(); i += 1){
    faces[i]->changeFrame(c0Mo);
    if(faces[i]->isVisible(c0Mo, angleAppears)){
      faces[i]->updateMask(mask, 255 - i*15, maskBorder);
    }
  }
  
  tracker.initTracking(cur, mask);

    // initialise the map of the points
  iPI0.clear();
  for (unsigned int i = 0; i < static_cast<unsigned int>(tracker.getNbFeatures()); i += 1){
    int id;
    float x_tmp, y_tmp;
    tracker.getFeature(i, id, x_tmp, y_tmp);
    vpImagePoint iP;
    iP.set_i(static_cast<double>(y_tmp));
    iP.set_j(static_cast<double>(x_tmp));
    iPI0[id] = iP; //id is the unique identifier for a point
  }
  
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(faces[i]->isVisible(c0Mo, angleAppears)){
      std::vector<vpImagePoint> roi;
      roi.resize(0);
      vpHomogeneousMatrix cMf;
      faces[i]->changeFrame(c0Mo);
      for (unsigned int j = 0; j < faces[i]->getNbPoint(); j += 1){
        vpImagePoint ip;
        vpPoint tmp = faces[i]->getPoint(j);
        vpMeterPixelConversion::convertPoint(cam, tmp.get_x(), tmp.get_y(), ip);
        roi.push_back(ip);
      }
      if(vpMbtKltPolygon::roiInsideImage(_I, roi)){
        faces[i]->init(iPI0, roi);
        faces[i]->setIsTracked(true);
      }
      else{
        faces[i]->setIsTracked(false);
      }
    }
    else{
      faces[i]->setIsTracked(false);
    }
  }

  cvReleaseImage(&mask);
}

/*!
  Set the camera parameters

  \param _cam : the new camera parameters
*/
void
vpMbKltTracker::setCameraParameters(const vpCameraParameters& _cam)
{
  for (unsigned int i = 0; i < faces.size(); i += 1){
    faces[i]->setCameraParameters(_cam);
  }
  this->cam = _cam;
  this->cameraInitialised = true;
}

/*!
  set the current pose.

  \param _cMo : the current pose.
*/
void vpMbKltTracker::setPose(const vpHomogeneousMatrix &_cMo)
{
  cMo = _cMo;
}
          
          
/*!
  Initialise a new face from the coordinates given in parameter.

  \param _corners : Coordinates of the corners of the face in the object frame.
  \param _indexFace : index of the face (depends on the vrml file organisation).
*/
void
vpMbKltTracker::initFaceFromCorners(const std::vector<vpPoint>& _corners, const unsigned int _indexFace)
{  
  vpMbtKltPolygon *polygon = new vpMbtKltPolygon;
  polygon->setCameraParameters(cam);
  polygon->setNbPoint(_corners.size());
  polygon->setIndex((int)_indexFace);
  for(unsigned int j = 0; j < _corners.size(); j++) {
    polygon->addPoint(j, _corners[j]);
  }
  faces.addPolygon(polygon);

  delete polygon;
  polygon = NULL;
}

/*!
  Realise the pre tracking operations

  \param _I : The input image.
  \param nbInfos : Size of the features.
  \param nbFaceUsed : Number of face used for the tracking.
*/
void
vpMbKltTracker::preTracking(const vpImage<unsigned char>& _I, unsigned int &nbInfos, unsigned int &nbFaceUsed)
{
  vpImageConvert::convert(_I,cur);
  tracker.track(cur);
  
  nbInfos = 0;  
  nbFaceUsed = 0;
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(faces[i]->getIsTracked()){
      faces[i]->computeNbDetectedCurrent(tracker);
          
//       faces[i]->ransac();
      if(faces[i]->hasEnoughPoints()){
        nbInfos += faces[i]->getNbPointsCur();
        nbFaceUsed++;
      }
    }
  }
}

/*!
  Realise the post tracking operations. Mostly visibility tests
*/
bool
vpMbKltTracker::postTracking(const vpImage<unsigned char>& _I, vpColVector &w)
{
  unsigned int shift = 0;
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(faces[i]->getIsTracked() && faces[i]->hasEnoughPoints()){
      vpSubColVector sub_w(w, shift, 2*faces[i]->getNbPointsCur());
      faces[i]->removeOutliers(sub_w, threshold_outlier);
      shift += 2*faces[i]->getNbPointsCur();
    }
  }
  
  bool reInitialisation = false;
  faces.setVisible(_I, cMo, angleAppears, angleDisappears, reInitialisation);

  if(reInitialisation)
    return true;
  
  return false;
}

/*!
  Realise the VVS loop for the tracking

  \param nbInfos : Size of the features
  \param w : weight of the features after M-Estimation.
*/
void
vpMbKltTracker::computeVVS(const unsigned int &nbInfos, vpColVector &w)
{
  vpMatrix J;     // interaction matrix
  vpColVector R;  // residu
  vpColVector v;  // "speed" for VVS
  vpHomography H;
  vpRobust robust(2*nbInfos);

  vpMatrix JTJ, JTR;
  
  double normRes = 0;
  double normRes_1 = -1;
  unsigned int iter = 0;

  R.resize(2*nbInfos);
  J.resize(2*nbInfos, 6, 0);
  
  while( ((int)((normRes - normRes_1)*1e8) != 0 )  && (iter<maxIter) ){
    
    unsigned int shift = 0;
    for (unsigned int i = 0; i < faces.size(); i += 1){
      if(faces[i]->getIsTracked() && faces[i]->hasEnoughPoints()){
        vpSubColVector subR(R, shift, 2*faces[i]->getNbPointsCur());
        vpSubMatrix subJ(J, shift, 0, 2*faces[i]->getNbPointsCur(), 6);
        try{
          faces[i]->computeHomography(ctTc0, H);
          faces[i]->computeInteractionMatrixAndResidu(subR, subJ);
        }catch(...){
          std::cerr << "exception while tracking face " << i << std::endl;
          throw ;
        }

        shift += 2*faces[i]->getNbPointsCur();
      }
    }

      /* robust */
    if(iter == 0){
      w.resize(2*nbInfos);
      w = 1;
    }
    robust.setIteration(iter);
    robust.setThreshold(2/cam.get_px());
    robust.MEstimator( vpRobust::TUKEY, R, w);

    normRes_1 = normRes;
    normRes = 0;
    for (unsigned int i = 0; i < static_cast<unsigned int>(R.getRows()); i += 1){
      R[i] *= w[i];
      normRes += R[i];
    }

    if((iter == 0) || compute_interaction){
      for(unsigned int i=0; i<static_cast<unsigned int>(R.getRows()); i++){
        for(unsigned int j=0; j<6; j++){
          J[i][j] *= w[i];
        }
      }
    }
    
    JTJ = J.AtA();
    computeJTR(J, R, JTR);
    v = -lambda * JTJ.pseudoInverse(1e-16) * JTR;
    
    ctTc0 = vpExponentialMap::direct(v).inverse() * ctTc0;
    
    iter++;
  }
  
  cMo = ctTc0 * c0Mo;
}

/*!
  Realise the tracking of the object in the image

  \throw vpException : if the tracking is supposed to have failed

  \param _I : the input image
*/
void
vpMbKltTracker::track(const vpImage<unsigned char>& _I)
{
  
  unsigned int nbInfos;
  unsigned int nbFaceUsed;
  preTracking(_I, nbInfos, nbFaceUsed);
  
  if(nbInfos < 4 || nbFaceUsed == 0){
    vpERROR_TRACE("\n\t\t Error-> not enough data") ;
    throw vpTrackingException(vpTrackingException::notEnoughPointError, "\n\t\t Error-> not enough data");
  }

  vpColVector w;
  computeVVS(nbInfos, w);  

  if(postTracking(_I, w))
    init(_I);
}

/*!
  Load the xml configuration file.
  From the configuration file parameters write initialize the corresponding objects (Ecm, camera).

  \warning To clean up memory allocated by the xml library, the user has to call
  vpXmlParser::cleanup() before the exit().

  \param _filename : full name of the xml file.
*/
void 
vpMbKltTracker::loadConfigFile(const std::string& _filename)
{
  vpMbKltTracker::loadConfigFile(_filename.c_str());
}

/*!
  Load the xml configuration file.
  From the configuration file parameters initialize the corresponding objects (Ecm, camera).

  \warning To clean up memory allocated by the xml library, the user has to call
  vpXmlParser::cleanup() before the exit().

  \throw vpException::ioError if the file has not been properly parsed (file not
  found or wrong format for the data). 

  \param filename : full name of the xml file.

  \sa vpXmlParser::cleanup()
*/
void
vpMbKltTracker::loadConfigFile(const char* filename)
{
#ifdef VISP_HAVE_XML2
  vpMbtKltXmlParser xmlp;
  
  xmlp.setMaxFeatures(10000);
  xmlp.setWindowSize(5);
  xmlp.setQuality(0.01);
  xmlp.setMinDistance(5);
  xmlp.setHarrisParam(0.01);
  xmlp.setBlockSize(3);
  xmlp.setPyramidLevels(3);
  xmlp.setMaskBorder(maskBorder);
  xmlp.setThresholdOutliers(threshold_outlier);
  xmlp.setAngleAppear(vpMath::deg(angleAppears));
  xmlp.setAngleDisappear(vpMath::deg(angleDisappears));
  
  try{
    std::cout << " *********** Parsing XML for MBT KLT Tracker ************ " << std::endl;
    
    xmlp.parse(filename);
  }
  catch(...){
    vpERROR_TRACE("Can't open XML file \"%s\"\n ",filename);
    throw vpException(vpException::ioError, "problem to parse configuration file.");
  }

  vpCameraParameters camera;
  xmlp.getCameraParameters(camera);
  setCameraParameters(camera); 
  
  tracker.setMaxFeatures(xmlp.getMaxFeatures());
  tracker.setWindowSize(xmlp.getWindowSize());
  tracker.setQuality(xmlp.getQuality());
  tracker.setMinDistance(xmlp.getMinDistance());
  tracker.setHarrisFreeParameter(xmlp.getHarrisParam());
  tracker.setBlockSize(xmlp.getBlockSize());
  tracker.setPyramidLevels(xmlp.getPyramidLevels());
  maskBorder = xmlp.getMaskBorder();
  threshold_outlier = xmlp.getThresholdOutliers();
  angleAppears = vpMath::rad(xmlp.getAngleAppear());
  angleDisappears = vpMath::rad(xmlp.getAngleDisappear());
  
#else
  vpTRACE("You need the libXML2 to read the config file %s", filename);
#endif
}

/*!
  Display the 3D model at a given position using the given camera parameters

  \param _I : The image .
  \param _cMo : Pose used to project the 3D model into the image.
  \param _cam : The camera parameters.
  \param _col : The desired color.
  \param _l : The thickness of the lines.
  \param displayFullModel : Boolean to say if all the model has to be displayed.
*/
void
vpMbKltTracker::display(const vpImage<unsigned char>& _I, const vpHomogeneousMatrix &_cMo, const vpCameraParameters &/*_cam*/, const vpColor& _col , const unsigned int _l, const bool displayFullModel)
{
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(displayFullModel || faces[i]->getIsTracked())
    {
      faces[i]->changeFrame(_cMo);
      for (unsigned int j = 0; j < faces[i]->getNbPoint(); j += 1){
        vpImagePoint ip1, ip2;
        ip1 = faces[i]->getImagePoint(j);
        ip2 = faces[i]->getImagePoint((j+1)%faces[i]->getNbPoint());
        vpDisplay::displayLine (_I, ip1, ip2, _col, _l);
      }
      
      if(faces[i]->hasEnoughPoints())
        faces[i]->displayPrimitive(_I);
      
//       if(facesTracker[i].hasEnoughPoints())
//         faces[i]->displayNormal(_I);
    }
  }
}

/*!
  Display the 3D model at a given position using the given camera parameters

  \param _I : The color image .
  \param _cMo : Pose used to project the 3D model into the image.
  \param _cam : The camera parameters.
  \param _col : The desired color.
  \param _l : The thickness of the lines.
  \param displayFullModel : Boolean to say if all the model has to be displayed.
*/
void
vpMbKltTracker::display(const vpImage<vpRGBa>& _I, const vpHomogeneousMatrix &_cMo, const vpCameraParameters &/*_cam*/, const vpColor& _col , const unsigned int _l, const bool displayFullModel)
{
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(displayFullModel || faces[i]->getIsTracked())
    {
      faces[i]->changeFrame(_cMo);
      for (unsigned int j = 0; j < faces[i]->getNbPoint(); j += 1){
        vpImagePoint ip1, ip2;
        ip1 = faces[i]->getImagePoint(j);
        ip2 = faces[i]->getImagePoint((j+1)%faces[i]->getNbPoint());
        vpDisplay::displayLine (_I, ip1, ip2, _col, _l);
      }
      
      if(faces[i]->hasEnoughPoints())
        faces[i]->displayPrimitive(_I);
      
//       if(facesTracker[i].hasEnoughPoints())
//         faces[i]->displayNormal(_I);
    }
  }
}

/*!
  Test the quality of the tracking.
  The tracking is supposed to fail if less than 10 points are tracked.

  \todo Find a efficient way to test the quality.

  \throw vpTrackingException::fatalError  if the test fails.
*/
void
vpMbKltTracker::testTracking()
{
  unsigned int nbTotalPoints = 0;
  for (unsigned int i = 0; i < faces.size(); i += 1){
    if(faces[i]->getIsTracked()){
      nbTotalPoints += faces[i]->getNbPointsCur();
    }
  }

  if(nbTotalPoints < 10){
    std::cerr << "test tracking failed (too few points to realise a good tracking)." << std::endl;
    throw vpTrackingException(vpTrackingException::fatalError,
          "test tracking failed (too few points to realise a good tracking).");
  }
}

#endif //VISP_HAVE_OPENCV
