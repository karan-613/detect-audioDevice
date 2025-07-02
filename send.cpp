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
    bool operator==(const deviceInfo& rhs) const {
        return dev_port == rhs.dev_port && dev_name == rhs.dev_name;
    }
};

struct services2__userData{
	pa_mainloop* mainloop;
    pa_context* context;

    std::string default_sink;
    std::string default_source;
    std::string monitor_of;
    bool is_monitor_valid = false;
    bool done = false;
    bool internal_change = false; //内部默认sink和source变更

};

std::vector<deviceInfo> s_currentPluggedDevices;

std::vector<deviceInfo> s_tempPluggedDevices;

void sink_info_callback(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
void event_callback1(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata);
void source_infoList_callback(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void writeSharedMemory(bool isFirstWrite);
static int modifyHeadPhoneState();
static void initPulseAudio();
void services2__server_info_cb(pa_context* c, const pa_server_info* i, void* userdata);

std::string services2__getCurrentTime() {
    time_t now = time(0);
    char timeStr[100];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return std::string(timeStr);
}

void services2__writeLog(const std::string& message, const std::string& filename = "/tmp/app.log") {
    std::ofstream logFile;

    // 以追加模式打开文件
    logFile.open(filename, std::ios::app);

    if (logFile.is_open()) {
        logFile << "[" << services2__getCurrentTime() << "] " << message << std::endl;
        logFile.close();
    } else {
        std::cerr << "无法打开日志文件: " << filename << std::endl;
    }
}


// PulseAudio 状态回调函数
void context_state_callback(pa_context *c, void *userdata)
{
    switch (pa_context_get_state(c))
    {
    case PA_CONTEXT_READY:{
        std::cout << "PulseAudio context is ready." << std::endl;
        services2__userData* data = static_cast<services2__userData*>(userdata);
        pa_operation* op = pa_context_get_server_info(c, services2__server_info_cb, data);
        if (op) pa_operation_unref(op);
        break;}
    case PA_CONTEXT_FAILED:
        std::cerr << "PulseAudio context failed." << std::endl;
        sleep(3);
        initPulseAudio();//这里没有结束pulseaudio，直接建立新的连接，会有内存泄漏
        break;
    case PA_CONTEXT_TERMINATED:
        std::cout << "PulseAudio context terminated." << std::endl;
        break;
    default:
        break;
    }
}

void services2__module1_load_cb(pa_context* c, uint32_t idx, void* userdata){
    auto* d = static_cast<services2__userData*>(userdata);
    if (idx == PA_INVALID_INDEX) {
        std::cerr << "module-lock-default-sink 加载失败" << std::endl;
        services2__writeLog("module-lock-default-sink 加载失败");
    }else{
        std::string str = "module-lock-default-sink 加载成功 (index=" + std::to_string(idx)+")";
        services2__writeLog(str);
        std::cout << "module-lock-default-sink 加载成功 (index="<< idx <<")" << std::endl;
    }
    d->done = true;
}

void services2__module_load_cb(pa_context* c, uint32_t idx, void* userdata) {
    auto* d = static_cast<services2__userData*>(userdata);
    if (idx == PA_INVALID_INDEX) {
        std::cerr << "module-elevoc-engine 加载失败" << std::endl;
        services2__writeLog("module-elevoc-engine 加载失败");
    } else {
        std::cout << "module-elevoc-engine 加载成功 (index="<< idx <<")，设置默认 Sink/Source…" << std::endl;
        std::string str = "module-elevoc-engine 加载成功 (index=" +std::to_string(idx)+")，设置默认 Sink/Source…";
        services2__writeLog(str);
        pa_context_set_default_sink(c, "echoCancelsink", nullptr, nullptr);
        pa_context_set_default_source(c, "noiseReduceSource", nullptr, nullptr);
        d->internal_change = true;
        const char* args = "sink_name=echoCancelsink";
        pa_operation* op = pa_context_load_module(c, "module-lock-default-sink", args, services2__module1_load_cb, userdata);
        if (op) pa_operation_unref(op);
    }
}

void services2__sink_info_cb(pa_context* c, const pa_sink_info* i, int eol, void* userdata){
    services2__userData* data = static_cast<services2__userData*>(userdata);
    if (eol < 0) {
        std::cerr << "Failed to get default sink info" << std::endl;
        return;
    }

    if(eol>0){
        return;
    }

    if (eol == 0 && i) {
        std::cout<<"default_sink->monitor_of_source "<<i->monitor_source_name<<std::endl;
        std::string str = "default_sink->monitor_of_source " + std::string(i->monitor_source_name);
        services2__writeLog(str);
        data->monitor_of = i->monitor_source_name;
        data->is_monitor_valid = data->default_source == data->monitor_of ? false : true;
    }
    pa_subscription_mask mask_t = PA_SUBSCRIPTION_MASK_SOURCE;
    if(data->is_monitor_valid){
        const char* args =
        "use_master_format=1 "
        "aec_method=elevoc "
        "aec_args=\"analog_gain_control=0 digital_gain_control=1\" "
        "source_name=noiseReduceSource "
        "sink_name=echoCancelsink "
        "rate=48000 "
        "format=float32le";
        pa_operation* op = pa_context_load_module(
            c,
            "module-elevoc-engine",
            args,
            services2__module_load_cb,
            userdata
        );
        if (op) pa_operation_unref(op);
    }else{
        mask_t = pa_subscription_mask(mask_t | PA_SUBSCRIPTION_MASK_SERVER);
    }
    // 订阅设备事件
    pa_context_subscribe(c, mask_t, nullptr, nullptr);
    pa_context_set_subscribe_callback(c, event_callback1, data);
}

void services2__server_info_cb(pa_context* c, const pa_server_info* i, void* userdata) {
    services2__userData* data = static_cast<services2__userData*>(userdata);

    if (!i) {
        std::cerr << "Failed to get server info" << std::endl;
        return;
    }

    data->default_sink = i->default_sink_name ? i->default_sink_name : "";
    data->default_source = i->default_source_name ? i->default_source_name : "";

    // std::cout << "Default Sink: " << data->default_sink << std::endl;
    std::cout << "Default Source: " << data->default_source << std::endl;

    services2__writeLog("Default Sink: "+std::string(data->default_sink));
    services2__writeLog("Default Source: " + std::string(data->default_source));

     // 获取默认 Sink 的详细信息
    pa_operation* op = pa_context_get_sink_info_by_name(c, data->default_sink.c_str(),
        services2__sink_info_cb,
        data
    );

    if (op) {
        pa_operation_unref(op);
    } else {
        std::cerr << "Failed to initiate sink info query" << std::endl;
    }

}

void event_callback1(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata)
{
    services2__userData* data = static_cast<services2__userData*>(userdata);
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
    if(facility == PA_SUBSCRIPTION_EVENT_SERVER && event_type == PA_SUBSCRIPTION_EVENT_CHANGE ){
        if(data->internal_change){
            return;
        }
        std::cout << "检测到服务器配置变更，重新验证..." << std::endl;
        services2__writeLog("检测到服务器配置变更，重新验证...");
        pa_operation* op = pa_context_get_server_info(c, services2__server_info_cb, data);
        if (op) pa_operation_unref(op);
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
        if(s_currentPluggedDevices == s_tempPluggedDevices){
            std::cout<<"The state is not change"<<std::endl;
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

    //创建services2 结构体
    services2__userData data{};
    data.mainloop = m;
    data.context = c;


    // 设置上下文状态回调
    pa_context_set_state_callback(c, context_state_callback, &data);

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
    std::cerr<<"the process overed."<<std::endl;
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