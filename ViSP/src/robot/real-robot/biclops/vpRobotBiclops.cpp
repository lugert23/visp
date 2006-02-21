/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 * Copyright Projet Lagadic / IRISA, 2006
 *+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 *
 * File:      vpRobotBiclops.cpp
 * Project:   Visp2
 * Author:    Fabien Spindler
 *
 * Version control
 * ===============
 *
 *  $Id: vpRobotBiclops.cpp,v 1.1 2006-02-21 11:16:11 fspindle Exp $
 *
 * Description
 * ============

 *
 * ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
#include <signal.h>
#include <unistd.h>

#include <visp/vpConfig.h>

#ifdef HAVE_ROBOT_BICLOPS_PT

#include <visp/vpBiclops.h>
#include <visp/vpRobotBiclops.h>
#include <visp/vpRobotException.h>
#include <visp/vpDebug.h>
#include <visp/vpExponentialMap.h>

/* ---------------------------------------------------------------------- */
/* --- STATIC ------------------------------------------------------------ */
/* ------------------------------------------------------------------------ */

bool vpRobotBiclops::robotAlreadyCreated = false;
const double vpRobotBiclops::defaultPositioningVelocity = 10.0;

static pthread_mutex_t vpEndThread_mutex;
static pthread_mutex_t vpShm_mutex;
static pthread_mutex_t vpMeasure_mutex;

/* ----------------------------------------------------------------------- */
/* --- CONSTRUCTOR ------------------------------------------------------ */
/* ---------------------------------------------------------------------- */


/*!

  Default constructor.

  Initialize the biclops pan, tilt head by reading the
  /usr/share/BiclopsDefault.cfg default configuration file provided by Traclabs
  and do the homing sequence.

  To change the default configuration file see setConfigFile().

*/
vpRobotBiclops::vpRobotBiclops (void)
  :
  vpRobot ()
{
  DEBUG_TRACE (12, "Begin default constructor.");

  sprintf(configfile, "/usr/share/BiclopsDefault.cfg");

  // Initialize the mutex dedicated to she shm protection
  pthread_mutex_init (&vpShm_mutex, NULL);
  pthread_mutex_init (&vpEndThread_mutex, NULL);
  pthread_mutex_init (&vpMeasure_mutex, NULL);

  DEBUG_TRACE (12, "Lock mutex vpMeasure_mutex");
  pthread_mutex_lock(&vpMeasure_mutex);

  init();

  try
  {
    setRobotState(vpRobot::STATE_STOP) ;
  }
  catch(...)
  {
    ERROR_TRACE(" ") ;
    throw ;
  }

  vpRobotBiclops::robotAlreadyCreated = true;

  positioningVelocity = defaultPositioningVelocity ;

  // Initialize previous articular position to manage getDisplacement()
  q_previous.resize(vpBiclops::ndof);
  q_previous = 0;

  controlThreadCreated = false;

  return ;
}


/*!

  Destructor.
  Wait the end of the control thread.

*/

vpRobotBiclops::~vpRobotBiclops (void)
{

  DEBUG_TRACE(12, "Start vpRobotBiclops::~vpRobotBiclops()");
  setRobotState(vpRobot::STATE_STOP) ;

  DEBUG_TRACE (12, "Unlock mutex vpEndThread_mutex");
  pthread_mutex_unlock(&vpEndThread_mutex);

  /* wait the end of the control thread */
  int code;
  DEBUG_TRACE (12, "Wait end of control thread");

  if (controlThreadCreated == true) {
    code = pthread_join(control_thread, NULL);
    if (code != 0) {
      CERROR << "Cannot terminate the control thread: " << code
	     << " strErr=" << strerror(errno)
	     << " strCode=" << strerror(code)
	     << endl;
    }
  }

  pthread_mutex_destroy (&vpShm_mutex);
  pthread_mutex_destroy (&vpEndThread_mutex);
  pthread_mutex_destroy (&vpMeasure_mutex);

  vpRobotBiclops::robotAlreadyCreated = false;

  DEBUG_TRACE(12, "Stop vpRobotBiclops::~vpRobotBiclops()");
  return;
}


/* ------------------------------------------------------------------------- */
/* --- INITIALISATION ------------------------------------------------------ */
/* ------------------------------------------------------------------------- */
/*!

  Set the Biclops config filename.

*/
void
vpRobotBiclops::setConfigFile(const char * filename)
{
  sprintf(configfile, "%s", filename);
}


/*!

  Check if the config file exists and initialize the head.

  \exception vpRobotException::constructionError If the config file cannot be
  oppened.

*/
void
vpRobotBiclops::init (void)
{

  // test if the config file exists
  FILE *fd = fopen(configfile, "r");
  if (fd == NULL) {
    CERROR << "Cannot open biclops config file: " << configfile << endl;
    throw vpRobotException (vpRobotException::constructionError,
 			    "Cannot open connexion with biclops");
  }
  fclose(fd);

  // Initialize the controller
  controller.init(configfile);

  return ;
}

/*
  Control loop to manage the biclops joint limits in speed control.

  This control loop is running in a seperate thread in order to detect each 5
  ms joint limits during the speed control. If a joint limit is detected the
  axis should be halted.

  \warning Velocity control mode is not exported from the top-level Biclops API
  class provided by Traclabs. That means that there is no protection in this
  mode to prevent an axis from striking its hard limit. In position mode,
  Traclabs put soft limits in that keep any command from driving to a position
  too close to the hard limits. In velocity mode this protection does not exist
  in the current API.

  \warning With the understanding that hitting the hard limits at full
  speed/power can damage the unit, damage due to velocity mode commanding is
  under user responsibility.
*/
void * vpRobotBiclops::vpRobotBiclopsSpeedControlLoop (void * arg)
{
  vpRobotBiclopsController *controller = ( vpRobotBiclopsController * ) arg;

  int iter = 0;
//   PMDAxisControl *panAxis  = controller->getPanAxis();
//   PMDAxisControl *tiltAxis = controller->getTiltAxis();
  vpRobotBiclopsController::shmType shm;

  DEBUG_TRACE(12, "Start control loop");
  vpColVector mes_q;
  vpColVector mes_q_dot;
  vpColVector softLimit(vpBiclops::ndof);
  vpColVector q_dot(vpBiclops::ndof);
  bool *new_q_dot  = new bool [ vpBiclops::ndof ];
  bool *change_dir = new bool [ vpBiclops::ndof ]; // change of direction
  bool *force_halt = new bool [ vpBiclops::ndof ]; // force an axis to halt
  bool *enable_limit = new bool [ vpBiclops::ndof ]; // enable soft limit
  vpColVector prev_q_dot(vpBiclops::ndof); // previous desired speed
  double secure = vpMath::rad(2); // add a security angle before joint limit


  // Set the soft limits
  softLimit[0] = vpBiclops::panJointLimit  - secure;
  softLimit[1] = vpBiclops::tiltJointLimit - secure;
  DEBUG_TRACE(12, "soft limit pan: %f tilt: %f",
	      vpMath::deg(softLimit[0]),
	      vpMath::deg(softLimit[1]));

  // Initilisation
  DEBUG_TRACE (12, "Lock mutex vpShm_mutex");
  pthread_mutex_lock(&vpShm_mutex);

  shm = controller->readShm();

  DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
  pthread_mutex_unlock(&vpShm_mutex);

  for (int i=0; i < vpBiclops::ndof; i ++) {
    prev_q_dot  [i] = shm.q_dot[i];
    new_q_dot   [i] = false;
    change_dir  [i] = false;
    force_halt  [i] = false;
    enable_limit[i] = true;
  }

  // Initialize actual position and velocity
  mes_q     = controller->getActualPosition();
  mes_q_dot = controller->getActualVelocity();

  shm = controller->readShm();
  // Updates the shm
  for (int i=0; i < vpBiclops::ndof; i ++) {
    shm.actual_q[i]     = mes_q[i];
    shm.actual_q_dot[i] = mes_q_dot[i];
  }
  // Update the actuals positions
  controller->writeShm(shm);

  DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
  pthread_mutex_unlock(&vpShm_mutex);

  DEBUG_TRACE (12, "unlock mutex vpMeasure_mutex");
  pthread_mutex_unlock(&vpMeasure_mutex); // A position is available

  while (1) {

    // Get actual position and velocity
    mes_q     = controller->getActualPosition();
    mes_q_dot = controller->getActualVelocity();

    DEBUG_TRACE (12, "Lock mutex vpShm_mutex");
    pthread_mutex_lock(&vpShm_mutex);


    shm = controller->readShm();

    // Updates the shm
    for (int i=0; i < vpBiclops::ndof; i ++) {
      shm.actual_q[i]     = mes_q[i];
      shm.actual_q_dot[i] = mes_q_dot[i];
    }

    DEBUG_TRACE(10, "mes pan: %f tilt: %f",
		vpMath::deg(mes_q[0]),
		vpMath::deg(mes_q[1]));
    DEBUG_TRACE(8, "mes pan vel: %f tilt vel: %f",
		vpMath::deg(mes_q_dot[0]),
		vpMath::deg(mes_q_dot[1]));
    DEBUG_TRACE(10, "desired  q_dot : %f %f",
		vpMath::deg(shm.q_dot[0]),
		vpMath::deg(shm.q_dot[1]));
    DEBUG_TRACE(10, "previous q_dot : %f %f",
		vpMath::deg(prev_q_dot[0]),
		vpMath::deg(prev_q_dot[1]));

    for (int i=0; i < vpBiclops::ndof; i ++) {
      // test if joint limits are reached
      if (mes_q[i] < -softLimit[i]) {
	DEBUG_TRACE(10, "Axe %d in low joint limit", i);
	shm.status[i] = vpRobotBiclopsController::STOP;
	shm.jointLimit[i] = true;
      }
      else if (mes_q[i] > softLimit[i]) {
	DEBUG_TRACE(10, "Axe %d in hight joint limit", i);
	shm.status[i] = vpRobotBiclopsController::STOP;
	shm.jointLimit[i] = true;
      }
      else {
	shm.status[i] = vpRobotBiclopsController::SPEED;
	shm.jointLimit[i] = false;
      }

      // Test if new a speed is demanded
      if (shm.q_dot[i] != prev_q_dot[i])
	new_q_dot[i] = true;
      else
	new_q_dot[i] = false;

      // Test if desired speed change of sign
      if ((shm.q_dot[i] * prev_q_dot[i]) < 0.)
	change_dir[i] = true;
      else
	change_dir[i] = false;

    }
    DEBUG_TRACE(10, "status      : %d %d", shm.status[0], shm.status[1]);
    DEBUG_TRACE(10, "joint       : %d %d", shm.jointLimit[0], shm.jointLimit[1]);
    DEBUG_TRACE(10, "new q_dot   : %d %d", new_q_dot[0], new_q_dot[1]);
    DEBUG_TRACE(10, "new dir     : %d %d", change_dir[0], change_dir[1]);
    DEBUG_TRACE(10, "force halt  : %d %d", force_halt[0], force_halt[1]);
    DEBUG_TRACE(10, "enable limit: %d %d", enable_limit[0], enable_limit[1]);


    bool updateVelocity = false;
    for (int i=0; i < vpBiclops::ndof; i ++) {
      // Test if a new desired speed is to apply
      if (new_q_dot[i]) {
	// A new desired speed is to apply
	if (shm.status[i] == vpRobotBiclopsController::STOP) {
	  // Axis in joint limit
	  if (change_dir[i] == false) {
	    // New desired speed without change of direction
	    // We go in the joint limit
	    if (enable_limit[i] == true) { // limit detection active
	      // We have to stop this axis
	      // Test if this axis was stopped before
	      if (force_halt[i] == false) {
		q_dot[i] = 0.;
		force_halt[i] = true; // indicate that it will be stopped
		updateVelocity = true; // We have to send this new speed
	      }
	    }
	    else {
	      // We have to apply the desired speed to go away the joint
	      // Update the desired speed
	      q_dot[i] = shm.q_dot[i];
	      shm.status[i] = vpRobotBiclopsController::SPEED;
	      force_halt[i] = false;
	      updateVelocity  = true; // We have to send this new speed
	    }
	  }
	  else {
	    // New desired speed and change of direction.
	    if (enable_limit[i] == true) { // limit detection active
	      // Update the desired speed to go away the joint limit
	      q_dot[i] = shm.q_dot[i];
	      shm.status[i] = vpRobotBiclopsController::SPEED;
	      force_halt[i] = false;
	      enable_limit[i] = false; // Disable joint limit detection
	      updateVelocity = true; // We have to send this new speed
	    }
	    else {
	      // We have to stop this axis
	      // Test if this axis was stopped before
	      if (force_halt[i] == false) {
		q_dot[i] = 0.;
		force_halt[i] = true; // indicate that it will be stopped
		enable_limit[i] = true; // Joint limit detection must be active
		updateVelocity  = true; // We have to send this new speed
	      }
	    }
	  }
	}
	else {
	  // Axis not in joint limit

	  // Update the desired speed
	  q_dot[i] = shm.q_dot[i];
	  shm.status[i] = vpRobotBiclopsController::SPEED;
	  enable_limit[i] = true; // Joint limit detection must be active
	  updateVelocity  = true; // We have to send this new speed
	}
      }
      else {
	// No change of the desired speed. We have to stop the robot in case of
	// joint limit
	if (shm.status[i] == vpRobotBiclopsController::STOP) {// axis limit
	  if (enable_limit[i] == true)  { // limit detection active

	    // Test if this axis was stopped before
	    if (force_halt[i] == false) {
	      // We have to stop this axis
	      q_dot[i] = 0.;
	      force_halt[i] = true; // indicate that it will be stopped
	      updateVelocity = true; // We have to send this new speed
	    }
	  }
	}
	else {
	  // No need to stop the robot
	  enable_limit[i] = true; // Normal situation, activate limit detection
	}
      }
    }
    // Update the actuals positions
    controller->writeShm(shm);

    DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
    pthread_mutex_unlock(&vpShm_mutex);

    if (updateVelocity) {
      DEBUG_TRACE(10, "apply q_dot : %f %f",
		vpMath::deg(q_dot[0]),
		vpMath::deg(q_dot[1]));

      // Apply the velocity
      controller -> setVelocity( q_dot );
    }


    // Update the previous speed for next iteration
    for (int i=0; i < vpBiclops::ndof; i ++)
      prev_q_dot[i] = shm.q_dot[i];

    DEBUG_TRACE(12, "iter: %d", iter);
    usleep(5000);

    if (pthread_mutex_trylock(&vpEndThread_mutex) == 0) {
      DEBUG_TRACE (12, "Calling thread will end");
      DEBUG_TRACE (12, "Unlock mutex vpEndThread_mutex");

      pthread_mutex_unlock(&vpEndThread_mutex);
      break;
    }

    iter ++;
  }

  // Stop the robot
  DEBUG_TRACE(12, "End of the control thread: stop the robot");
  q_dot = 0;
  controller -> setVelocity( q_dot );

  delete [] new_q_dot;
  delete [] change_dir;
  delete [] force_halt;
  delete [] enable_limit;

  DEBUG_TRACE (12, "Exit control thread ");
  //  pthread_exit(0);

  return NULL;
}


/*!

  Change the state of the robot either to stop them, or to set position or
  speed control.

*/
vpRobot::RobotStateType
vpRobotBiclops::setRobotState(vpRobot::RobotStateType newState)
{
  switch (newState)
  {
  case vpRobot::STATE_STOP:
    {
      if (vpRobot::STATE_STOP != getRobotState ())
      {
	stopMotion();
      }
      break;
    }
  case vpRobot::STATE_POSITION_CONTROL:
    {
      if (vpRobot::STATE_VELOCITY_CONTROL == getRobotState ())
      {
	DEBUG_TRACE (12, "Speed to position control.");
	stopMotion();
      }

      break;
    }
  case vpRobot::STATE_VELOCITY_CONTROL:
    {

      if (vpRobot::STATE_VELOCITY_CONTROL != getRobotState ())
      {
	DEBUG_TRACE (12, "Lock mutex vpEndThread_mutex");
	pthread_mutex_lock(&vpEndThread_mutex);

	DEBUG_TRACE (12, "Create speed control thread");
	int code;
	code = pthread_create(&control_thread, NULL,
			      &vpRobotBiclops::vpRobotBiclopsSpeedControlLoop,
			      &controller);
	if (code != 0)  {
	  CERROR << "Cannot create speed biclops control thread: " << code
		 << " strErr=" << strerror(errno)
		 << " strCode=" << strerror(code)
		 << endl;
	}

	controlThreadCreated = true;

	DEBUG_TRACE (12, "Speed control thread created");
      }
      break;
    }
  default:
    break ;
  }

  return vpRobot::setRobotState (newState);
}



/*!

  Halt all the axis.

*/
void
vpRobotBiclops::stopMotion(void)
{
  vpColVector q_dot(vpBiclops::ndof);
  q_dot = 0;
  controller.setVelocity(q_dot);

}

/*!

  Get the twist matrix corresponding to the transformation between the
  camera frame and the end effector frame. The end effector frame is located on
  the tilt axis.

  \param cVe : Twist transformation between camera and end effector frame to
  expess a velocity skew from end effector frame in camera frame.

*/
void
vpRobotBiclops::get_cVe(vpTwistMatrix &cVe)
{
  vpHomogeneousMatrix cMe ;
  vpBiclops::get_cMe(cMe) ;

  cVe.buildFrom(cMe) ;
}

/*!

  Get the homogeneous matrix corresponding to the transformation between the
  camera frame and the end effector frame. The end effector frame is located on
  the tilt axis.

  \param cMe :  Homogeneous matrix between camera and end effector frame.

*/
void
vpRobotBiclops::get_cMe(vpHomogeneousMatrix &cMe)
{
  vpBiclops::get_cMe(cMe) ;
}


/*!
  Get the robot jacobian expressed in the end-effector frame.

  \warning Re is not the embedded camera frame. It corresponds to the frame
  associated to the tilt axis (see also get_cMe).

  \param eJe : Jacobian between end effector frame and end effector frame (on
  tilt axis).

*/
void
vpRobotBiclops::get_eJe(vpMatrix &eJe)
{
  vpColVector q(2) ;
  getPosition(vpRobot::ARTICULAR_FRAME, q) ;

  try
  {
    vpBiclops::get_eJe(q,eJe) ;
  }
  catch(...)
  {
    ERROR_TRACE("catch exception ") ;
    throw ;
  }
}

/*!
  Get the robot jacobian expressed in the robot reference frame

  \param fJe : Jacobian between reference frame (or fix frame) and end effector
  frame (on tilt axis).

*/
void
vpRobotBiclops::get_fJe(vpMatrix &fJe)
{
  vpColVector q(2) ;
  getPosition(vpRobot::ARTICULAR_FRAME, q) ;

  try
  {
    vpBiclops::get_fJe(q,fJe) ;
  }
  catch(...)
  {
    throw ;
  }

}



/*!

  Set the velocity for a positionning task.

  \param velocity : Velocity in % of the maximum velocity between [0,100]. The
  maximum velocity is given vpBiclops::speedLimit.
*/
void
vpRobotBiclops::setPositioningVelocity (const double velocity)
{
  if (velocity < 0 || velocity > 100) {
    ERROR_TRACE("Bad positionning velocity");
    throw vpRobotException (vpRobotException::constructionError,
			    "Bad positionning velocity");
  }

  positioningVelocity = velocity;
}
/*!
  Get the velocity in % for a positionning task.

  \return Positionning velocity in [0, 100.0]. The
  maximum positionning velocity is given vpBiclops::speedLimit.

*/
double
vpRobotBiclops::getPositioningVelocity (void)
{
  return positioningVelocity;
}


/*!
   Move the robot in position control.

   \warning This method is blocking. That mean that it waits the end of the
   positionning.

   \param frame : Control frame. This biclops head can only be controlled in
   articular.

   \param q : The position to set for each axis in radians.

   \exception vpRobotException::wrongStateError : If a not supported frame type
   is given.

*/
void
vpRobotBiclops::setPosition (const vpRobot::ControlFrameType frame,
			     const vpColVector & q )
{

  if (vpRobot::STATE_POSITION_CONTROL != getRobotState ())
  {
    ERROR_TRACE ("Robot was not in position-based control\n"
		 "Modification of the robot state");
    setRobotState(vpRobot::STATE_POSITION_CONTROL) ;
  }

  switch(frame)
  {
  case vpRobot::CAMERA_FRAME:
    ERROR_TRACE ("Cannot move the robot in camera frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot move the robot in camera frame: "
			    "not implemented");
    break;
  case vpRobot::REFERENCE_FRAME:
    ERROR_TRACE ("Cannot move the robot in reference frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot move the robot in reference frame: "
			    "not implemented");
    break;
  case vpRobot::MIXT_FRAME:
    ERROR_TRACE ("Cannot move the robot in mixt frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot move the robot in mixt frame: "
			    "not implemented");
    break;
  case vpRobot::ARTICULAR_FRAME:
    break ;
  }

  // test if position reachable
//   if ( (fabs(q[0]) > vpBiclops::panJointLimit) ||
//        (fabs(q[1]) > vpBiclops::tiltJointLimit) ) {
//     ERROR_TRACE ("Positionning error.");
//     throw vpRobotException (vpRobotException::wrongStateError,
// 			    "Positionning error.");
//   }
  controller.setPosition( q, positioningVelocity );

  return ;
}

/*!
   Move the robot in position control.

   \warning This method is blocking. That mean that it wait the end of the
   positionning.

   \param frame : Control frame. This biclops head can only be controlled in
   articular.

   \param q1 : The pan position to set in radians.
   \param q2 : The tilt position to set in radians.

   \exception vpRobotException::wrongStateError : If a not supported frame type
   is given.

*/
void vpRobotBiclops::setPosition (const vpRobot::ControlFrameType frame,
				  const double &q1, const double &q2)
{
  try{
    vpColVector q(2) ;
    q[0] = q1 ;
    q[1] = q2 ;

    setPosition(frame,q) ;
  }
  catch(...)
  {
    throw ;
  }
}

/*!

  Read the content of the position file and moves to head to articular
  position.

  \param filename : Position filename

  \exception vpRobotException::readingParametersError : If the articular
  position cannot be read from file.

  \sa readPositionFile()

*/
void
vpRobotBiclops::setPosition(const char *filename)
{
  vpColVector q ;
  if (readPositionFile(filename, q) == false) {
    ERROR_TRACE ("Cannot get biclops position from file");
    throw vpRobotException (vpRobotException::readingParametersError,
			    "Cannot get biclops position from file");
  }
  setPosition ( vpRobot::ARTICULAR_FRAME, q) ;
}

/*!

  Return the position of each axis.
  - In positionning control mode, call vpRobotBiclopsController::getPosition()
  - In speed control mode, call vpRobotBiclopsController::getActualPosition()

  \param frame : Control frame. This biclops head can only be controlled in
  articular.

  \param q : The position of the axis in radians.

  \exception vpRobotException::wrongStateError : If a not supported frame type
  is given.

*/
void
vpRobotBiclops::getPosition (const vpRobot::ControlFrameType frame,
			     vpColVector & q)
{
  switch(frame)
  {
  case vpRobot::CAMERA_FRAME :
    ERROR_TRACE ("Cannot get position in camera frame: not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in camera frame: "
			    "not implemented");
    break;
  case vpRobot::REFERENCE_FRAME:
    ERROR_TRACE ("Cannot get position in reference frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in reference frame: "
			    "not implemented");
    break;
  case vpRobot::MIXT_FRAME:
    ERROR_TRACE ("Cannot get position in mixt frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in mixt frame: "
			    "not implemented");
    break;
  case vpRobot::ARTICULAR_FRAME:
    break ;
  }

  vpRobot::RobotStateType state;
  state = vpRobot::getRobotState();

  switch (state) {
  case STATE_STOP:
  case STATE_POSITION_CONTROL:
    q = controller.getPosition();

    break;
  case STATE_VELOCITY_CONTROL:
  case STATE_ACCELERATION_CONTROL:
  default:
    q.resize(vpBiclops::ndof);

    DEBUG_TRACE (12, "unlock mutex vpMeasure_mutex");
    pthread_mutex_lock(&vpMeasure_mutex); // Wait until a position is available

    vpRobotBiclopsController::shmType shm;

    DEBUG_TRACE (12, "Lock mutex vpShm_mutex");
    pthread_mutex_lock(&vpShm_mutex);

    shm = controller.readShm();

    DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
    pthread_mutex_unlock(&vpShm_mutex);

    for (int i=0; i < vpBiclops::ndof; i ++) {
      q[i] = shm.actual_q[i];
    }

    CDEBUG(11) << "++++++++ Measure actuals: " << q.t();

    DEBUG_TRACE (12, "unlock mutex vpMeasure_mutex");
    pthread_mutex_unlock(&vpMeasure_mutex); // A position is available

    break;

  }
}


/*!

  Send a velocity on each axis.

  \param frame : Control frame. This biclops head can only be controlled in
  articular.

  \param q_dot : The desired velocity of the axis in radians.

  \exception vpRobotException::wrongStateError : If a not supported frame type
  is given.
*/
void
vpRobotBiclops::setVelocity (const vpRobot::ControlFrameType frame,
			     const vpColVector & q_dot)
{
  if (vpRobot::STATE_VELOCITY_CONTROL != getRobotState ())
  {
    ERROR_TRACE ("Cannot send a velocity to the robot "
		 "use setRobotState(vpRobot::STATE_VELOCITY_CONTROL) first) ");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot send a velocity to the robot "
			    "use setRobotState(vpRobot::STATE_VELOCITY_CONTROL) first) ");
  }

  switch(frame)
  {
  case vpRobot::CAMERA_FRAME :
    {
      ERROR_TRACE ("Cannot send a velocity to the robot "
		   "in the camera frame: "
		   "functionality not implemented");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot send a velocity to the robot "
			      "in the camera frame:"
			      "functionality not implemented");
      break ;
    }
  case vpRobot::ARTICULAR_FRAME :
    {
      if ( q_dot.getRows() != 2) {
	ERROR_TRACE ("Bad dimension fo speed vector in articular frame");
	throw vpRobotException (vpRobotException::wrongStateError,
				"Bad dimension for speed vector "
				"in articular frame");
      }
      break ;
    }
  case vpRobot::REFERENCE_FRAME :
    {
      ERROR_TRACE ("Cannot send a velocity to the robot "
		   "in the reference frame: "
		   "functionality not implemented");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot send a velocity to the robot "
			      "in the reference frame:"
			      "functionality not implemented");
      break ;
    }
  case vpRobot::MIXT_FRAME :
    {
      ERROR_TRACE ("Cannot send a velocity to the robot "
		   "in the mixt frame: "
		   "functionality not implemented");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot send a velocity to the robot "
			      "in the mixt frame:"
			      "functionality not implemented");
      break ;
    }
  default:
    {
      ERROR_TRACE ("Error in spec of vpRobot. "
		   "Case not taken in account.");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot send a velocity to the robot ");
    }
  }

  DEBUG_TRACE (12, "Velocity limitation.");

  // Saturate articular speed
  double max = vpBiclops::speedLimit;
  for (int i = 0 ; i < vpBiclops::ndof; ++ i) // q1 and q2
  {
    if (fabs (q_dot[i]) > max)
    {
      max = fabs (q_dot[i]);
      ERROR_TRACE ("Excess velocity: ROTATION "
		     "(axe nr.%d).", i);
    }
  }
  max = vpBiclops::speedLimit / max;

  vpColVector q_dot_sat = q_dot * max;

  CDEBUG(12) << "send velocity: " << q_dot_sat.t() << endl;

  vpRobotBiclopsController::shmType shm;

  DEBUG_TRACE (12, "Lock mutex vpShm_mutex");
  pthread_mutex_lock(&vpShm_mutex);

  shm = controller.readShm();

  for (int i=0; i < vpBiclops::ndof; i ++)
    shm.q_dot[i] = q_dot_sat[i];

  controller.writeShm(shm);

  DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
  pthread_mutex_unlock(&vpShm_mutex);

  return;
}


/* ------------------------------------------------------------------------- */
/* --- GET ----------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */


/*!

  Get the articular velocity.

  \param frame : Control frame. This head can only be controlled in articular.

  \param q_dot : The measured articular velocity in rad/s.

  \exception vpRobotException::wrongStateError : If a not supported frame type
  is given.
*/
void
vpRobotBiclops::getVelocity (const vpRobot::ControlFrameType frame,
			     vpColVector & q_dot)
{
  switch(frame)
  {
  case vpRobot::CAMERA_FRAME :
    ERROR_TRACE ("Cannot get position in camera frame: not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in camera frame: "
			    "not implemented");
    break;
  case vpRobot::REFERENCE_FRAME:
    ERROR_TRACE ("Cannot get position in reference frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in reference frame: "
			    "not implemented");
    break;
  case vpRobot::MIXT_FRAME:
    ERROR_TRACE ("Cannot get position in mixt frame: "
		 "not implemented");
    throw vpRobotException (vpRobotException::wrongStateError,
			    "Cannot get position in mixt frame: "
			    "not implemented");
    break;
  case vpRobot::ARTICULAR_FRAME:
    break ;
  }

  vpRobot::RobotStateType state;
  state = vpRobot::getRobotState();

  switch (state) {
  case STATE_STOP:
  case STATE_POSITION_CONTROL:
    q_dot = controller.getVelocity();

    break;
  case STATE_VELOCITY_CONTROL:
  case STATE_ACCELERATION_CONTROL:
  default:
    q_dot.resize(vpBiclops::ndof);

    DEBUG_TRACE (12, "unlock mutex vpMeasure_mutex");
    pthread_mutex_lock(&vpMeasure_mutex); // Wait until a position is available

    vpRobotBiclopsController::shmType shm;

    DEBUG_TRACE (12, "Lock mutex vpShm_mutex");
    pthread_mutex_lock(&vpShm_mutex);

    shm = controller.readShm();

    DEBUG_TRACE (12, "unlock mutex vpShm_mutex");
    pthread_mutex_unlock(&vpShm_mutex);

    for (int i=0; i < vpBiclops::ndof; i ++) {
      q_dot[i] = shm.actual_q_dot[i];
    }

    CDEBUG(11) << "++++++++ Velocity actuals: " << q_dot.t();

    DEBUG_TRACE (12, "unlock mutex vpMeasure_mutex");
    pthread_mutex_unlock(&vpMeasure_mutex); // A position is available

    break;

  }
}


/*!

  Return the articular velocity.

  \param frame : Control frame. This head can only be controlled in articular.

  \return The measured articular velocity in rad/s.

  \exception vpRobotException::wrongStateError : If a not supported frame type
  is given.
*/
vpColVector
vpRobotBiclops::getVelocity (vpRobot::ControlFrameType frame)
{
  vpColVector q_dot;
  getVelocity (frame, q_dot);

  return q_dot;
}

/*!

  Get an articular position from the position file.

  \param filename : Position file.

  \param q : The articular position read in the file.

  \code
  # Example of biclops position file
  # The axis positions must be preceed by R:
  # First value : pan  articular position in degrees
  # Second value: tilt articular position in degrees
  R: 15.0 5.0
  \endcode

  \return true if a position was found, false otherwise.

*/
bool
vpRobotBiclops::readPositionFile(const char *filename, vpColVector &q)
{
  FILE * pt_f ;
  pt_f = fopen(filename,"r") ;

  if (pt_f == NULL) {
    ERROR_TRACE ("Can not open biclops position file %s", filename);
    return false;
  }

  char line[FILENAME_MAX];
  char head[] = "R:";
  bool end = false;

  do {
    // skip lines begining with # for comments
    if (fgets (line, 100, pt_f) != NULL) {
      if ( strncmp (line, "#", 1) != 0) {
	// this line is not a comment
	if ( fscanf (pt_f, "%s", line) != EOF)   {
	  if ( strcmp (line, head) == 0)
	    end = true; 	// robot position was found
	}
	else
	  return (false); // end of file without position
      }
    }
    else {
      return (false);// end of file
    }

  }
  while ( end != true );

  double q1,q2;
  // Read positions
  fscanf(pt_f, "%lf %lf", &q1, &q2);
  q.resize(vpBiclops::ndof) ;

  q[0] = vpMath::rad(q1) ; // Rot tourelle
  q[1] = vpMath::rad(q2) ;

  fclose(pt_f) ;
  return (true);
}

/*!

  Get the robot displacement expressed in the camera frame since the last call
  of this method.

  \param d The measured displacement in camera frame. The dimension of d is 6
  (tx, ty, ty, rx, ry, rz). Translations are expressed in meters, rotations in
  radians.

  \sa getDisplacement(), getArticularDisplacement()

*/
void
vpRobotBiclops::getCameraDisplacement(vpColVector &d)
{
  getDisplacement(vpRobot::CAMERA_FRAME, d);

}
/*!

  Get the robot articular displacement since the last call of this method.

  \param d The measured articular displacement. The dimension of d is 2 (the
  number of axis of the robot) with respectively d[0] (pan displacement),
  d[1] (tilt displacement)

  \sa getDisplacement(), getCameraDisplacement()

*/
void vpRobotBiclops::getArticularDisplacement(vpColVector &d)
{
  getDisplacement(vpRobot::ARTICULAR_FRAME, d);
}

/*!

  Get the robot displacement since the last call of this method.

  \warning The first call of this method gives not a good value for the
  displacement.

  \param frame The frame in which the measured displacement is expressed.

  \param d The displacement:

  - In articular, the dimension of q is 2  (the number of axis of the robot)
  with respectively d[0] (pan displacement), d[1] (tilt displacement).

  - In camera frame, the dimension of d is 6 (tx, ty, ty, tux, tuy, tuz).
  Translations are expressed in meters, rotations in radians with the theta U
  representation.

  \exception vpRobotException::wrongStateError If a not supported frame type is
  given.

  \sa getArticularDisplacement(), getCameraDisplacement()

*/
void
vpRobotBiclops::getDisplacement(vpRobot::ControlFrameType frame,
				vpColVector &d)
{
  vpColVector q_current; // current position

  getPosition(vpRobot::ARTICULAR_FRAME, q_current);

  switch(frame) {
  case vpRobot::ARTICULAR_FRAME:
    d.resize(vpBiclops::ndof);
    d = q_current - q_previous;
    break ;

  case vpRobot::CAMERA_FRAME: {
    d.resize(6);
    vpHomogeneousMatrix fMc_current;
    vpHomogeneousMatrix fMc_previous;
    fMc_current  = vpBiclops::computeMGD(q_current);
    fMc_previous = vpBiclops::computeMGD(q_previous);
    vpHomogeneousMatrix c_previousMc_current;
    // fMc_c = fMc_p * c_pMc_c
    // => c_pMc_c = (fMc_p)^-1 * fMc_c
    c_previousMc_current = fMc_previous.inverse() * fMc_current;

    // Compute the instantaneous velocity from this homogeneous matrix.
    d = vpExponentialMap::inverse( c_previousMc_current );
    break ;
  }

  case vpRobot::REFERENCE_FRAME:
      ERROR_TRACE ("Cannot get a velocity in the reference frame: "
		   "functionality not implemented");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot get a velocity in the reference frame:"
			      "functionality not implemented");
      break ;
  case vpRobot::MIXT_FRAME:
      ERROR_TRACE ("Cannot get a velocity in the mixt frame: "
		   "functionality not implemented");
      throw vpRobotException (vpRobotException::wrongStateError,
			      "Cannot get a velocity in the mixt frame:"
			      "functionality not implemented");
      break ;
  }


  q_previous = q_current; // Update for next call of this method

}



/*
 * Local variables:
 * c-basic-offset: 2
 * End:
 */

#endif

