#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <pulse/pulseaudio.h>
#include <thread>
#include <chrono>
#include <string.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <csignal>

#include "json.hpp"

using json = nlohmann::json;


static int shm_fd;
static char* shm_ptr=nullptr;
sem_t* sem_write = nullptr;
static json j;
static const char* SHM_NAME = "/shared_mem_pulseaudio";
static const char* SEM_WRITE_NAME = "/my_sem_write";
static const char* SEM_READ_NAME = "/my_sem_read";

static const char *DEVICES = "devices";
static const char *CURRENTDEVNAME = "currentDevicesName";
static const char *CURRENTDEVCATE = "currentDevicesCategory";
static const char* DEFAULTDEVNAME= "defaultDeviceName";
static const char* DEFAULTDEVCATE = "defaultDeviceCategory";

static bool bremoveOnce = true; // 插拔设备只触发一次
static const char *USBHeadphone_bus = "usb";
static const char *Headphone_bus = "pci";
static const char *InputInternalMic = "analog-input-internal-mic";
static const char *InputMic = "analog-input-mic";
static std::string defaultSourceName = "内部话筒(HD-Audio Generic)";
static int defaultDevCategory = 2;//内置麦克风

static enum EnumHeadphone {
    USB,
    POINT35MM,
    INTERNAL, // 内置
    BT,       // 蓝牙
} s_enHeadPhone;

struct deviceInfo
{
    std::string dev_name;
    std::string dev_bus;
    std::string dev_port;
    std::string dev_description;
    bool isDefaultDevice = false;
    EnumHeadphone dev_category = INTERNAL;
    deviceInfo(std::string _dev_name,
               std::string _dev_bus,
               std::string _dev_port,
               std::string _dev_description,
               bool _isDefaultDevice = false,
               EnumHeadphone _dev_category = INTERNAL) : dev_name(_dev_name),
                                                         dev_bus(_dev_bus),
                                                         dev_port(_dev_port),
                                                         dev_description(_dev_description),
                                                         isDefaultDevice(_isDefaultDevice),
                                                         dev_category(_dev_category) {}
};

std::vector<deviceInfo> s_currentPluggedDevices;

std::vector<deviceInfo> s_tempPluggedDevices;

void sink_info_callback(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
void event_callback1(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata);
void source_infoList_callback(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void writeSharedMemory(bool isFirstWrite);
static int modifyHeadPhoneState();
static void initPulseAudio();


// PulseAudio 状态回调函数
void context_state_callback(pa_context *c, void *userdata)
{
    switch (pa_context_get_state(c))
    {
    case PA_CONTEXT_READY:
        std::cout << "PulseAudio context is ready." << std::endl;
        // 订阅设备事件
        pa_context_subscribe(c, pa_subscription_mask(PA_SUBSCRIPTION_MASK_SOURCE), nullptr, nullptr);
        pa_context_set_subscribe_callback(c, event_callback1, nullptr);
        break;
    case PA_CONTEXT_FAILED:
        std::cerr << "PulseAudio context failed." << std::endl;
        sleep(3);
        initPulseAudio();
        break;
    case PA_CONTEXT_TERMINATED:
        std::cout << "PulseAudio context terminated." << std::endl;
        break;
    default:
        break;
    }
}

void event_callback1(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata)
{

    pa_subscription_event_type_t facility = pa_subscription_event_type_t(type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
    pa_subscription_event_type_t event_type = pa_subscription_event_type_t(type & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

    // 如果是新设备或设备改变，获取设备信息

    pa_operation *op = nullptr;
    if ((event_type == PA_SUBSCRIPTION_EVENT_CHANGE || event_type == PA_SUBSCRIPTION_EVENT_NEW || event_type == PA_SUBSCRIPTION_EVENT_REMOVE) && bremoveOnce)
    {
        // 插拔设备
        bremoveOnce = false;
        std::cout << "=====PA_SUBSCRIPTION_EVENT_SOURCE=====" << std::endl;
        s_tempPluggedDevices.clear();
        op = pa_context_get_source_info_list(c, source_infoList_callback, nullptr);
    }
    if (op)
    {
        pa_operation_unref(op);
    }
}

void source_infoList_callback(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    if (eol)
    {
        bremoveOnce = true;
        if (s_tempPluggedDevices.empty())
        {
            return;
        }
        s_currentPluggedDevices = s_tempPluggedDevices;
        for (const auto &i : s_currentPluggedDevices)
        {
            std::cout << "i.dev_port:" << i.dev_port << std::endl;
            std::cout << "i.dev_name:" << i.dev_name << std::endl;
            std::cout << "i.dev_bus:" << i.dev_bus << std::endl;
            std::cout << "i.dev_category:" << i.dev_category << std::endl;
        }
        // modifyHeadPhoneState();
        writeSharedMemory(false);
        return; // 列表结束
    }

    const char *dev_port = (i->active_port ? i->active_port->name : "N/A");
    if (dev_port == "N/A")
    {
        std::cout << "port not available1" << std::endl;
        return;
    }
    std::string dev_description_prev = i->active_port->description ? i->active_port->description : "N/A";
    std::string dev_description_next = pa_proplist_gets(i->proplist, "alsa.card_name");
    std::string dev_description = dev_description_prev + "(" + dev_description_next + ")";
    const char *dev_bus = pa_proplist_gets(i->proplist, PA_PROP_DEVICE_BUS);
    const char *dev_name = i->name ? i->name : "N/A";
    if (dev_bus==NULL || strcmp(dev_bus, Headphone_bus) == 0)
    {
        if (strcmp(dev_port, InputInternalMic) == 0)
        {
            s_enHeadPhone = EnumHeadphone::INTERNAL;
        }
        else
        {
            s_enHeadPhone = EnumHeadphone::POINT35MM;
        }
        defaultSourceName = dev_description.c_str();
        defaultDevCategory = static_cast<int>(s_enHeadPhone);
    }
    else if (strcmp(dev_bus, USBHeadphone_bus) == 0)
    {
        s_enHeadPhone = EnumHeadphone::USB;
    }
    else
    {
        std::cout << "cannot find Device Bus" << std::endl;
        return;
    }
    
    if(dev_bus==NULL){
        dev_bus=Headphone_bus;
    }
    s_tempPluggedDevices.push_back(deviceInfo(dev_name, dev_bus, dev_port, dev_description, "false", s_enHeadPhone));

}


static void save_data(){
    std::string jsonPath = "/etc/pulse/elevoc_devices.json";
    std::ofstream jsonFile(jsonPath);
    if (!jsonFile.is_open())
    {
        std::cout<<"Cannot open json file"<<std::endl;
        return ;
    }
    jsonFile<<j.dump(4);
}

static void closeSharedMemory(){
    // 解除映射和关闭
    sem_close(sem_write);
    munmap(shm_ptr, 1024);
    close(shm_fd);
}

static void signal_handler(int signal){
    std::cout<<"Received signal"<<signal<<", saving data..."<<std::endl;
    save_data();
    closeSharedMemory();
    exit(0);
}

static json createJsonData(){
    j[DEVICES]=json::object();
    j[DEVICES][CURRENTDEVCATE] = json::array();
    j[DEVICES][CURRENTDEVNAME] = json::array();
    j.clear();
    for (const auto &i : s_currentPluggedDevices)
    {
        j[DEVICES][CURRENTDEVCATE].push_back(static_cast<int>(i.dev_category));
        j[DEVICES][CURRENTDEVNAME].push_back(i.dev_description);
    }
    j[DEVICES][DEFAULTDEVNAME]=defaultSourceName;
    j[DEVICES][DEFAULTDEVCATE]=defaultDevCategory;

    return j;
}

static int modifyHeadPhoneState()
{
    std::string jsonPath = "/etc/pulse/elevoc_devices.json";
    std::ifstream jsonFile(jsonPath);
    if (!jsonFile.is_open())
    {
        // log_error("Cannot open json file:%s",jsonPath);
        std::cout<<"Cannot open json file"<<std::endl;
        return -1;
    }
    json j;
    try
    {
        jsonFile >> j;
        if(!j.contains(DEVICES)){
            j[DEVICES]=json::object();
        }
        if(!j[DEVICES].contains(CURRENTDEVCATE)){
            j[DEVICES][CURRENTDEVCATE] = json::array();
        }
        if(!j[DEVICES].contains(CURRENTDEVNAME)){
            j[DEVICES][CURRENTDEVNAME] = json::array();
        }
        j[DEVICES][CURRENTDEVCATE].clear();
        j[DEVICES][CURRENTDEVNAME].clear();
        for (const auto &i : s_currentPluggedDevices)
        {
            j[DEVICES][CURRENTDEVCATE].push_back(static_cast<int>(i.dev_category));
            j[DEVICES][CURRENTDEVNAME].push_back(i.dev_description);
        }
        j[DEVICES][DEFAULTDEVNAME]=defaultSourceName;
        j[DEVICES][DEFAULTDEVCATE]=defaultDevCategory;
    }
    catch (const nlohmann::json::parse_error &e)
    {
        std::cout<<"Init jsonFile failed, Parse error: "<<e.what();
        // log_error("Init jsonFile failed, Parse error: %s",e.what());
    }
    jsonFile.close();

    std::ofstream outputJsonFile(jsonPath);
    if (!outputJsonFile.is_open())
    {
        std::cout << "Failed to open file for writing." << std::endl;
        // log_error("Failed to open file for writing.");
        return -1;
    }
    outputJsonFile << j.dump(4);

    // log_info("The Headphone is update:%s", flag?"on":"off");
    return 0;
}

static void initPulseAudio(){
    s_enHeadPhone = EnumHeadphone::INTERNAL;
    s_currentPluggedDevices.push_back(deviceInfo("N/A", "N/A", "N/A", "N/A", false, EnumHeadphone::INTERNAL));

    // 创建 PulseAudio 主循环
    pa_mainloop *m = pa_mainloop_new();
    pa_mainloop_api *mainloop_api = pa_mainloop_get_api(m);
    pa_context *c = pa_context_new(mainloop_api, "Audio Device Monitor");

    // 设置上下文状态回调
    pa_context_set_state_callback(c, context_state_callback, nullptr);

    // 连接到 PulseAudio
    if (pa_context_connect(c, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
    {
        std::cerr << "Failed to connect to PulseAudio: " << pa_strerror(pa_context_errno(c)) << std::endl;
        return ;
    }

    // 运行主循环
    pa_mainloop_run(m, nullptr);

    // 断开 PulseAudio 连接并清理
    pa_context_disconnect(c);
    pa_context_unref(c);
    pa_mainloop_free(m);
}

static void initSharedMemory(){
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Failed to create shared memory!" << std::endl;
        return ;
    }

    // 设置共享内存大小
    ftruncate(shm_fd, 1024);

    // 映射共享内存
    shm_ptr = (char*) mmap(0, 1024, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        std::cerr << "Failed to map shared memory!" << std::endl;
        return ;
    }

    sem_write = sem_open(SEM_WRITE_NAME,O_CREAT,0666,0);
}

static void writeSharedMemory(bool isFirstWrite){

    if(sem_write == SEM_FAILED){
        perror("sem_open");
        return ;
    }

    // // 创建 JSON 对象
    if(!isFirstWrite){
        createJsonData();
    }

    // 将 JSON 序列化并写入共享内存
    std::string json_str = j.dump();
    std::cout<<"json_str:"<<json_str<<std::endl;
    memset(shm_ptr,0,json_str.size()+1);
    memcpy(shm_ptr, json_str.c_str(), json_str.size() + 1);


    std::cout << "write data shared memory successs" << std::endl;

    // 等待接收进程
    sem_post(sem_write);
}

static void firstWriteSharedMemory(){
    std::string jsonPath = "/etc/pulse/elevoc_devices.json";
    std::ifstream jsonFile(jsonPath);
    if (!jsonFile.is_open())
    {
        std::cout<<"Cannot open json file"<<std::endl;
        return ;
    }

    jsonFile>>j;
    writeSharedMemory(true);
}

int main()
{
    if(signal(SIGTERM,signal_handler)==SIG_ERR){
        std::cerr<<"unable to catch sigterm"<<std::endl;
    }

    if(signal(SIGINT,signal_handler)==SIG_ERR){
        std::cerr<<"unable to catch sigint"<<std::endl;
    }

    // std::shared_ptr<FILE> pipe(popen("systemctl --user restart pulseaudio.socket pulseaudio.service && pulseaudio --k","r"),pclose);
    // if (!pipe) {
    //     std::cerr<<"======popen() failed!======";
    // }
    // sleep(1);
    // while(pclose(pipe.get())!=0){
    //     sleep(1);
    // }
    initSharedMemory();
    firstWriteSharedMemory();
    initPulseAudio();
    return 0;
}