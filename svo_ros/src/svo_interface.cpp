#include <svo_ros/svo_interface.h>

#include <ros/callback_queue.h>

#include <svo_ros/svo_factory.h>
#include <svo_ros/visualizer.h>
#include <svo/common/frame.h>
#include <svo/map.h>
#include <svo/imu_handler.h>
#include <svo/common/camera.h>
#include <svo/common/conversions.h>
#include <svo/frame_handler_mono.h>
#include <svo/frame_handler_stereo.h>
#include <svo/frame_handler_array.h>
#include <svo/initialization.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <image_transport/subscriber_filter.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/image_encodings.h>
#include <cv_bridge/cv_bridge.h>
#include <vikit/params_helper.h>
#include <vikit/timer.h>

#ifdef SVO_USE_BACKEND
#include <svo/backend/backend_factory.h>
#include <svo/backend/backend_interface.h>
#include <svo/backend/backend_visualizer.h>
#include <svo/backend/backend_optimizer.h>
#endif

namespace svo {

SvoInterface::SvoInterface(
    const PipelineType& pipeline_type,
    const ros::NodeHandle& nh,
    const ros::NodeHandle& private_nh)
  : nh_(nh)
  , pnh_(private_nh)
  , pipeline_type_(pipeline_type)
  , set_initial_attitude_from_gravity_(
      vk::param<bool>(pnh_, "set_initial_attitude_from_gravity", true))
  , automatic_reinitialization_(
      vk::param<bool>(pnh_, "automatic_reinitialization", false))
{
  switch (pipeline_type)
  {
    case PipelineType::kMono:
      svo_ = factory::makeMono(pnh_);
      break;
    case PipelineType::kStereo:
      svo_ = factory::makeStereo(pnh_);
      break;
    case PipelineType::kArray:
      svo_ = factory::makeArray(pnh_);
      break;
    default:
      LOG(FATAL) << "Unknown pipeline";
      break;
  }
  ncam_ = svo_->getNCamera();
  visualizer_.reset(
        new Visualizer(svo_->options_.trace_dir, pnh_, ncam_->getNumCameras()));
  if(vk::param<bool>(pnh_, "use_imu", false))
    imu_handler_ = factory::getImuHandler(pnh_);

#ifdef SVO_USE_BACKEND
  if(vk::param<bool>(pnh_, "use_backend", false))
  {
    std::cout << "===================== SVO is using backend !!! ======================" << std::endl << std::endl << std::endl;
    backend_interface_ = svo::backend_factory::makeBackend(pnh_);
    backend_visualizer_.reset(new BackendVisualizer(svo_->options_.trace_dir, pnh_));
    svo_->setBundleAdjuster(backend_interface_);
    backend_interface_->imu_handler_ = imu_handler_;
  }
#endif

  svo_->start();
}

SvoInterface::~SvoInterface()
{
  //
  
  if (imu_thread_)
    imu_thread_->join();
  if (image_thread_)
    image_thread_->join();
  VLOG(1) << "Destructed SVO.";
}

void SvoInterface::processImageBundle(
    const std::vector<cv::Mat>& images,
    const int64_t timestamp_nanoseconds)
{
  svo_->addImageBundle(images, timestamp_nanoseconds);
}

void SvoInterface::publishResults(
    const std::vector<cv::Mat>& images,
    const int64_t timestamp_nanoseconds)
{
  CHECK_NOTNULL(svo_.get());
  CHECK_NOTNULL(visualizer_.get());
  visualizer_->publishSvoInfo(svo_.get(), timestamp_nanoseconds);
  switch (svo_->stage())
  {
    case Stage::kTracking: {
      Eigen::Matrix<double, 6, 6> Covariance; // TODO
      visualizer_->publishImuPose(
            svo_->getLastFrames()->get_T_W_B(), Covariance, timestamp_nanoseconds);
      visualizer_->publishCameraPoses(svo_->getLastFrames(), timestamp_nanoseconds);
 
      visualizer_->publishImagesWithFeatures(
            svo_->getLastFrames(), timestamp_nanoseconds);
      visualizer_->visualizeMarkers(
            svo_->getLastFrames(), svo_->closeKeyframes(), svo_->map());
      visualizer_->exportToDense(svo_->getLastFrames());
      
#ifdef LOGGING_LMK_LIFE
      // Added by Yipu
      if(svo_->getLastFrames()->isKeyframe())
	       visualizer_->grabAllLmkLog(svo_->map());
#endif
      
      break;
    }
    case Stage::kInitializing: {
      visualizer_->publishBundleFeatureTracks(
            svo_->initializer_->frames_ref_, svo_->getLastFrames(), timestamp_nanoseconds);
      break;
    }
    case Stage::kPaused:
    case Stage::kRelocalization:
      visualizer_->publishImages(images, timestamp_nanoseconds);
      break;
    default:
      LOG(FATAL) << "Unknown stage";
      break;
  }

#ifdef SVO_USE_BACKEND
  if(svo_->stage() == Stage::kTracking && backend_interface_)
  {
    if(svo_->getLastFrames()->isKeyframe())
    {
      std::lock_guard<std::mutex> estimate_lock(backend_interface_->optimizer_->estimate_mut_);
      const gtsam::Values& state = backend_interface_->optimizer_->estimate_;
      backend_visualizer_->visualizeFrames(state);
      if(backend_interface_->options_.add_imu_factors)
        backend_visualizer_->visualizeVelocity(state);
      backend_visualizer_->visualizePoints(state);
      backend_visualizer_->publishPointcloud(state);
      //vin_visualizer_->visualizeSmartFactors(ba_->graph_manager_->smart_factors_map_);
    }
  }
#endif
}

void SvoInterface::setImuPrior(const int64_t timestamp_nanoseconds)
{
  if(imu_handler_ && !svo_->hasStarted() && set_initial_attitude_from_gravity_)
  {
    // set initial orientation
    Quaternion R_imu_world;
    if(imu_handler_->getInitialAttitude(
         timestamp_nanoseconds * common::conversions::kNanoSecondsToSeconds,
         R_imu_world))
    {
      VLOG(3) << "Set initial orientation from accelerometer measurements.";
      svo_->setRotationPrior(R_imu_world);
    }
  }
  else if(imu_handler_ && svo_->getLastFrames())
  {
    // set incremental rotation prior
    ImuMeasurements imu_measurements;
    if(imu_handler_->getMeasurements(
         svo_->getLastFrames()->getMinTimestampNanoseconds() *
         common::conversions::kNanoSecondsToSeconds,
         timestamp_nanoseconds * common::conversions::kNanoSecondsToSeconds,
         false, imu_measurements))
    {
      Quaternion R_lastimu_newimu;
      if(imu_handler_->getRelativeRotationPrior(
           svo_->getLastFrames()->getMinTimestampNanoseconds() *
           common::conversions::kNanoSecondsToSeconds,
           timestamp_nanoseconds * common::conversions::kNanoSecondsToSeconds,
           false, R_lastimu_newimu))
      {
        VLOG(3) << "Set incremental rotation prior from IMU.";
        svo_->setRotationIncrementPrior(R_lastimu_newimu);
      }
    }
  }
}

void SvoInterface::monoCallback(const sensor_msgs::ImageConstPtr& msg)
{
  if(idle_)
    return;

  cv::Mat image;
  try
  {
    image = cv_bridge::toCvCopy(msg)->image;
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
  }
  
  
  double start_time = (double) cv::getTickCount();
  

  std::vector<cv::Mat> images;
  images.push_back(image.clone());

  setImuPrior(msg->header.stamp.toNSec());

  imageCallbackPreprocessing(msg->header.stamp.toNSec());

  processImageBundle(images, msg->header.stamp.toNSec());
    
  
  double time_1 = ((double) cv::getTickCount() - start_time)/cv::getTickFrequency();
  // std::cout  << "TIme cost of processImageBundle = " << time_1 * 1000 << std::endl;

  publishResults(images, msg->header.stamp.toNSec());

  if (svo_->stage() == Stage::kTracking)
  {
      Transformation tmp_T = svo_->getLastFrames()->get_T_W_C();
      Position tmp_P = tmp_T.getPosition();
      Eigen::Quaterniond tmp_Q = tmp_T.getEigenQuaternion();
      	      logFramePose.push_back( trackLog( msg->header.stamp.toSec(), 
					    tmp_P(0,0), 
					    tmp_P(1,0), 
					    tmp_P(2,0), 
					    tmp_Q.x(),
					    tmp_Q.y(),
					    tmp_Q.z(),
					    tmp_Q.w() ) );
	
	      /*
std::cout << "Published camera pose: "
	      << tmp_P(0,0) << " " << tmp_P(1,0) << " " << tmp_P(2,0) << "; "
	      << tmp_Q.x() << " " << tmp_Q.y() << " " << tmp_Q.z() << " " << tmp_Q.w() << std::endl;
	      */
  }	      

  if(svo_->stage() == Stage::kPaused && automatic_reinitialization_)
    svo_->start();

  imageCallbackPostprocessing();

double time_2 = ((double) cv::getTickCount() - start_time)/cv::getTickFrequency();
//  std::cout  << "TIme cost of publish and post processing  = " << time_2 * 1000 << std::endl;

logTimeCost.push_back(timeLog(msg->header.stamp.toSec(), time_1, time_2));

}

void SvoInterface::stereoCallback(
    const sensor_msgs::ImageConstPtr& msg0,
    const sensor_msgs::ImageConstPtr& msg1)
{
  if(idle_)
    return;


  cv::Mat img0, img1;
  try {
    img0 = cv_bridge::toCvShare(msg0, "mono8")->image;
    img1 = cv_bridge::toCvShare(msg1, "mono8")->image;
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
  }
  
  
double start_time = (double) cv::getTickCount();



  setImuPrior(msg0->header.stamp.toNSec());

  imageCallbackPreprocessing(msg0->header.stamp.toNSec());

  processImageBundle({img0, img1}, msg0->header.stamp.toNSec());
  
  
  double time_1 = ((double) cv::getTickCount() - start_time)/cv::getTickFrequency();
  // std::cout  << "TIme cost of processImageBundle = " << time_1 * 1000 << std::endl;

  
  
  publishResults({img0, img1}, msg0->header.stamp.toNSec());
  
  
  
  Transformation tmp_T = svo_->getLastFrames()->get_T_W_C();
      Position tmp_P = tmp_T.getPosition();
      Eigen::Quaterniond tmp_Q = tmp_T.getEigenQuaternion();
      	      logFramePose.push_back( trackLog( msg0->header.stamp.toSec(), 
					    tmp_P(0,0), 
					    tmp_P(1,0), 
					    tmp_P(2,0), 
					    tmp_Q.x(),
					    tmp_Q.y(),
					    tmp_Q.z(),
					    tmp_Q.w() ) );
	
	      /*
std::cout << "Published camera pose: "
	      << tmp_P(0,0) << " " << tmp_P(1,0) << " " << tmp_P(2,0) << "; "
	      << tmp_Q.x() << " " << tmp_Q.y() << " " << tmp_Q.z() << " " << tmp_Q.w() << std::endl;
	      */
	      
	      


  if(svo_->stage() == Stage::kPaused && automatic_reinitialization_)
    svo_->start();

  imageCallbackPostprocessing();


double time_2 = ((double) cv::getTickCount() - start_time)/cv::getTickFrequency();
//  std::cout  << "TIme cost of publish and post processing  = " << time_2 * 1000 << std::endl;

logTimeCost.push_back(timeLog(msg0->header.stamp.toSec(), time_1, time_2));

}

void SvoInterface::imuCallback(const sensor_msgs::ImuConstPtr& msg)
{
  const Eigen::Vector3d omega_imu(
        msg->angular_velocity.x, msg->angular_velocity.y, msg->angular_velocity.z);
  const Eigen::Vector3d lin_acc_imu(
        msg->linear_acceleration.x, msg->linear_acceleration.y, msg->linear_acceleration.z);
  const ImuMeasurement m(msg->header.stamp.toSec(), omega_imu, lin_acc_imu);
  if(imu_handler_)
    imu_handler_->addImuMeasurement(m);
  else
    SVO_ERROR_STREAM("SvoNode has no ImuHandler");
}

void SvoInterface::inputKeyCallback(const std_msgs::StringConstPtr& key_input)
{
  std::string remote_input = key_input->data;
  char input = remote_input.c_str()[0];
  switch(input)
  {
    case 'q':
      quit_ = true;
      SVO_INFO_STREAM("SVO user input: QUIT");
      break;
    case 'r':
      svo_->reset();
      idle_ = true;
      SVO_INFO_STREAM("SVO user input: RESET");
      break;
    case 's':
      svo_->start();
      idle_ = false;
      SVO_INFO_STREAM("SVO user input: START");
      break;
    default: ;
  }
}

void SvoInterface::subscribeImu()
{
  imu_thread_ = std::unique_ptr<std::thread>(
        new std::thread(&SvoInterface::imuLoop, this));
  sleep(3);
}

void SvoInterface::subscribeImage()
{
  if(pipeline_type_ == PipelineType::kMono)
    image_thread_ = std::unique_ptr<std::thread>(
          new std::thread(&SvoInterface::monoLoop, this));
  else if(pipeline_type_ == PipelineType::kStereo)
    image_thread_ = std::unique_ptr<std::thread>(
        new std::thread(&SvoInterface::stereoLoop, this));
}

void SvoInterface::subscribeRemoteKey()
{
  std::string remote_key_topic =
      vk::param<std::string>(pnh_, "remote_key_topic", "svo/remote_key");
  sub_remote_key_ =
      nh_.subscribe(remote_key_topic, 5, &svo::SvoInterface::inputKeyCallback, this);
}

void SvoInterface::imuLoop()
{
  SVO_INFO_STREAM("SvoNode: Started IMU loop.");
  ros::NodeHandle nh;
  ros::CallbackQueue queue;
  nh.setCallbackQueue(&queue);
  std::string imu_topic = vk::param<std::string>(pnh_, "imu_topic", "imu");
  ros::Subscriber sub_imu =
      nh.subscribe(imu_topic, 10, &svo::SvoInterface::imuCallback, this);
  while(ros::ok() && !quit_)
  {
    queue.callAvailable(ros::WallDuration(0.1));
  }
}

void SvoInterface::monoLoop()
{
  SVO_INFO_STREAM("SvoNode: Started Image loop.");

  ros::NodeHandle nh;
  ros::CallbackQueue queue;
  nh.setCallbackQueue(&queue);

  image_transport::ImageTransport it(nh);
  std::string image_topic =
      vk::param<std::string>(pnh_, "cam0_topic", "camera/image_raw");
  image_transport::Subscriber it_sub =
      it.subscribe(image_topic, 5, &svo::SvoInterface::monoCallback, this);

  while(ros::ok() && !quit_)
  {
    queue.callAvailable(ros::WallDuration(0.1));
  }
}

void SvoInterface::stereoLoop()
{
  typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, sensor_msgs::Image> ExactPolicy;
  typedef message_filters::Synchronizer<ExactPolicy> ExactSync;

  ros::NodeHandle nh(nh_, "image_thread");
  ros::CallbackQueue queue;
  nh.setCallbackQueue(&queue);

  // subscribe to cam msgs
  std::string cam0_topic(vk::param<std::string>(pnh_, "cam0_topic", "/cam0/image_raw"));
  std::string cam1_topic(vk::param<std::string>(pnh_, "cam1_topic", "/cam1/image_raw"));
  image_transport::ImageTransport it(nh);
  image_transport::SubscriberFilter sub0(it, cam0_topic, 1, std::string("raw"));
  image_transport::SubscriberFilter sub1(it, cam1_topic, 1, std::string("raw"));
  ExactSync sync_sub(ExactPolicy(5), sub0, sub1);
  sync_sub.registerCallback(boost::bind(&svo::SvoInterface::stereoCallback, this, _1, _2));

  while(ros::ok() && !quit_)
  {
    queue.callAvailable(ros::WallDuration(0.1));
  }
}

void SvoInterface::saveLmkLog(const std::string &filename) {

    CHECK_NOTNULL(svo_.get());
    CHECK_NOTNULL(visualizer_.get());
  //  visualizer_->grabAllLmkLog(svo_->map());
    size_t N = visualizer_->logLmkLife.size();
    std::cout << std::endl << "Saving " << N << " records to lmk log file " << filename << " ..." << std::endl;

    std::ofstream fLmkLog;
    fLmkLog.open(filename.c_str());
    fLmkLog << std::fixed;
    fLmkLog << "#id life" << std::endl;
    for(size_t i=0; i<N; i++)
    {
	fLmkLog << std::setprecision(0)
		      << visualizer_->logLmkLife[i].id << " "
		      << visualizer_->logLmkLife[i].life << std::endl;
    }
    fLmkLog.close();

    std::cout << "Finished saving lmk log! " << std::endl;
}

//
void SvoInterface::saveTimeLog(const string &filename) {

    std::cout << std::endl << "Saving " << this->logTimeCost.size() << " records to time log file " << filename << " ..." << std::endl;

    std::ofstream fFrameTimeLog;
    fFrameTimeLog.open(filename.c_str());
    fFrameTimeLog << std::fixed;
    fFrameTimeLog << "#frame_time_stamp time_pre_proc time_post_proc" << std::endl;
    for(size_t i=0; i<this->logTimeCost.size(); i++)
    {
        fFrameTimeLog << std::setprecision(6)
                      << this->logTimeCost[i].time_stamp << " "
                      << this->logTimeCost[i].time_cost_1 << " "
                      << this->logTimeCost[i].time_cost_2 << std::endl;
    }
    fFrameTimeLog.close();

    std::cout << "Finished saving log! " << std::endl;
}


void SvoInterface::saveAllFrameTrack(const std::string &filename) {
  
  std::cout << std::endl << "Saving " << this->logFramePose.size() << " records to track file " << filename << " ..." << std::endl;

    std::ofstream f_realTimeTrack;
    f_realTimeTrack.open(filename.c_str());
    f_realTimeTrack << std::fixed;
    f_realTimeTrack << "#TimeStamp Tx Ty Tz Qx Qy Qz Qw" << std::endl;
    for(size_t i=0; i<this->logFramePose.size(); i++)
    {
      f_realTimeTrack << std::setprecision(6)
			<< this->logFramePose[i].time_stamp << " "
			<< std::setprecision(7)
			<< this->logFramePose[i].position.transpose() << " "
			<< this->logFramePose[i].orientation.transpose() << std::endl;
    }
    f_realTimeTrack.close();

    std::cout << "Finished saving track! " << std::endl;
}


} // namespace svo
