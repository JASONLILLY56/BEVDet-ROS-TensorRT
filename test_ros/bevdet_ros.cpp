#include "bevdet_ros.h"

std::map< int, std::vector<int>> colormap { 
            {0, {0, 0, 255}},  // dodger blue 
            {1, {0, 201, 87}},   // 青色
            {2, {0, 201, 87}},
            {3, {160, 32, 240}},
            {4, {3, 168, 158}},
            {5, {255, 0, 0}},
            {6, {255, 97, 0}},
            {7, {30,  0, 255}},
            {8, {255, 0, 0}},
            {9, {0, 0, 255}},
            {10, {0, 0, 0}}
};

void Getinfo(void) 
{
    cudaDeviceProp prop;

    int count = 0;
    cudaGetDeviceCount(&count);
    printf("\nGPU has cuda devices: %d\n", count);
    for (int i = 0; i < count; ++i) {
        cudaGetDeviceProperties(&prop, i);
        printf("----device id: %d info----\n", i);
        printf("  GPU : %s \n", prop.name);
        printf("  Capbility: %d.%d\n", prop.major, prop.minor);
        printf("  Global memory: %luMB\n", prop.totalGlobalMem >> 20);
        printf("  Const memory: %luKB\n", prop.totalConstMem >> 10);
        printf("  Shared memory in a block: %luKB\n", prop.sharedMemPerBlock >> 10);
        printf("  warp size: %d\n", prop.warpSize);
        printf("  threads in a block: %d\n", prop.maxThreadsPerBlock);
        printf("  block dim: (%d,%d,%d)\n", prop.maxThreadsDim[0],
                prop.maxThreadsDim[1], prop.maxThreadsDim[2]);
        printf("  grid dim: (%d,%d,%d)\n", prop.maxGridSize[0], prop.maxGridSize[1],
                prop.maxGridSize[2]);
    }
    printf("\n");
}


void Boxes2Txt(const std::vector<Box> &boxes, std::string file_name, bool with_vel=false) 
{
  std::ofstream out_file;
  out_file.open(file_name, std::ios::out);
  if (out_file.is_open()) {
    for (const auto &box : boxes) {
      out_file << box.x << " ";
      out_file << box.y << " ";
      out_file << box.z << " ";
      out_file << box.l << " ";
      out_file << box.w << " ";
      out_file << box.h << " ";
      out_file << box.r << " ";
      if(with_vel)
      {
        out_file << box.vx << " ";
        out_file << box.vy << " ";
      }
      out_file << box.score << " ";
      out_file << box.label << "\n";
    }
  }
  out_file.close();
  return;
};


void Egobox2Lidarbox(const std::vector<Box>& ego_boxes, 
                                        std::vector<Box> &lidar_boxes,
                                        const Eigen::Quaternion<float> &lidar2ego_rot,
                                        const Eigen::Translation3f &lidar2ego_trans)
{
    
    for(size_t i = 0; i < ego_boxes.size(); i++){
        Box b = ego_boxes[i];
        Eigen::Vector3f center(b.x, b.y, b.z);
        center -= lidar2ego_trans.translation();
        center = lidar2ego_rot.inverse().matrix() * center;
        b.r -= lidar2ego_rot.matrix().eulerAngles(0, 1, 2).z();
        b.x = center.x();
        b.y = center.y();
        b.z = center.z();
        lidar_boxes.push_back(b);
    }
}

RosNode::RosNode()
{
    pkg_path_ = ros::package::getPath("bevdet");
    std::string config_path = pkg_path_ + "/cfgs/config.yaml";
    config_ = YAML::LoadFile(config_path);
    printf("Successful load config : %s!\n", config_path.c_str());
    bool testNuscenes = config_["TestNuscenes"].as<bool>();

    img_N_ = config_["N"].as<size_t>();  // 图片数量 6
    img_w_ = config_["W"].as<int>();        // H: 900
    img_h_ = config_["H"].as<int>();        // W: 1600
    
    // 模型配置文件路径 
    model_config_ = pkg_path_ + "/" + config_["ModelConfig"].as<std::string>();
    
    // 权重文件路径 图像部分 bev部分
    imgstage_file_ =pkg_path_ + "/" +  config_["ImgStageEngine"].as<std::string>();
    bevstage_file_ =pkg_path_ +"/" +  config_["BEVStageEngine"].as<std::string>();
    
    // 相机的内参配置参数
    camconfig_ = YAML::LoadFile(pkg_path_ +"/" + config_["CamConfig"].as<std::string>()); 
    // 结果保存文件
    output_lidarbox_ = pkg_path_ +"/" + config_["OutputLidarBox"].as<std::string>();
    
    sample_ = config_["sample"];

    for(auto file : sample_)
    {
        imgs_file_.push_back(pkg_path_ +"/"+ file.second.as<std::string>());
        imgs_name_.push_back(file.first.as<std::string>()); 
    }

    // 读取图像参数
    sampleData_.param = camParams(camconfig_, img_N_, imgs_name_);
    
    // 模型配置文件，图像数量，cam内参，cam2ego的旋转和平移，模型权重文件
    bevdet = std::make_shared<BEVDet>(model_config_, img_N_, sampleData_.param.cams_intrin, 
                sampleData_.param.cams2ego_rot, sampleData_.param.cams2ego_trans, 
                                                    imgstage_file_, bevstage_file_);
    
    
    // gpu分配内参， cuda上分配6张图的大小 每个变量sizeof(uchar)个字节，并用imgs_dev指向该gpu上内存, sizeof(uchar) =1
    CHECK_CUDA(cudaMalloc((void**)&imgs_dev_, img_N_ * 3 * img_w_ * img_h_ * sizeof(uchar)));

    
    pub_cloud_ = n_.advertise<sensor_msgs::PointCloud2>("/points_raw", 10); 
    pub_boxes_ = n_.advertise<jsk_recognition_msgs::BoundingBoxArray>("/boxes", 10);   
    sub_img_ = n_.subscribe<sensor_msgs::PointCloud2>("/camera/depth/points", 10, &RosNode::callback, this);
  
}

void RosNode::callback(const sensor_msgs::PointCloud2ConstPtr &msg)
{   
    std::string cloud_path = pkg_path_ + "/sample0/0.pcd";
    pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>);
    pcl::io::loadPCDFile(cloud_path, *cloud);
   

    std::vector<std::vector<char>> imgs_data;
    // // file读取到cpu
    // read_sample(imgs_file, imgs_data);
    // opencv读取BG  RGBHWC_to_BGRCHW
    read_sample_cv(imgs_file_, imgs_data);

    // 拷贝从cpu上imgs_data拷贝到gpu上imgs_dev
    // std::vector<std::vector<char>> imgs_data 并进行通道转换
    decode_cpu(imgs_data, imgs_dev_, img_w_, img_h_);

    // uchar *imgs_dev 已经到gpu上了数据
    sampleData_.imgs_dev = imgs_dev_;

    std::vector<Box> ego_boxes;
    ego_boxes.clear();
    float time = 0.f;
    // 测试推理  图像数据, boxes，时间
    bevdet->DoInfer(sampleData_, ego_boxes, time);
    
    std::vector<Box> lidar_boxes;
    
    // box从ego坐标变化到雷达坐标
    Egobox2Lidarbox(ego_boxes, lidar_boxes, sampleData_.param.lidar2ego_rot, 
                                            sampleData_.param.lidar2ego_trans);

    
    // std::string file_name
    // Boxes2Txt(lidar_boxes, output_lidarbox_, false);

     std::cout << cloud->points.size() << std::endl;

    sensor_msgs::PointCloud2 msg_cloud;
    pcl::toROSMsg(*cloud, msg_cloud);

    msg_cloud.header.frame_id = "map";
    msg_cloud.header.stamp = ros::Time::now();
    pub_cloud_.publish(msg_cloud);


    


}

RosNode::~RosNode()
{
    delete imgs_dev_;
}



int main(int argc, char **argv)
{   
    ros::init(argc, argv, "bevdet_node");
    // Getinfo(); # 打印信息
    
    auto bevdet_node = std::make_shared<RosNode>();
    ros::spin();
    return 0;
}