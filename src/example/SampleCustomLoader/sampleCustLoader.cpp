/*
 * Copyright (C) 2020-2021 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <chrono>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <assert.h>
#include <rapidjson/document.h>
#include <spdlog/spdlog.h>

#include "../../customloaderinterface.hpp"

//using namespace std;
using namespace rapidjson;
using namespace ovms;

#define PATH_SIZE 10
#define RSIZE_MAX_STR 4096

#define SAMPLE_LOADER_OK 0
#define SAMPLE_LOADER_ERROR 0x10

#define SAMPLE_LOADER_IR_MODEL   0
#define SAMPLE_LOADER_ONNX_MODEL 1
#define SAMPLE_LOADER_BLOB_MODEL 2

// Time in seconds at which model status will be checked
#define MODEL_CHECK_PERIOD 10
typedef std::pair<std::string, int> model_id_t;

/* 
 * This class implements am example custom model loader for OVMS.
 * It derives the implementation from base class CustomLoaderInterface
 * defined in ovms. The purpose this example is to demonistrate the 
 * usage of various APIs defined in base class, parse loader specic
 * parameters from the config file. 
 *
 * It reads the model files and returns the buffers to be loaded by the 
 * model server.  
 *
 * Also, based on the contents on <model>.status file, it black lists the model
 * or removes the model from blacklisting. During the periodic check on model 
 * loader will unload/reload model based on blacklist.
 */

class custSampleLoader : public CustomLoaderInterface {
private:
    std::vector<model_id_t> models_loaded;
    std::map<model_id_t,std::string> models_enable;
    std::map<model_id_t,bool> models_blacklist;
    std::mutex map_mutex;

protected:
    int extract_input_params(const std::string& basePath, int version, const std::string& loaderOptions,
    			std::string& binFile, std::string& modelFile, std::string& enableFile, int& modelType);

    int load_files(std::string& binFile, std::string& modelFile, int modelType, char** modelBuffer, 
		    char** binBuffer, int* modelLen, int* binLen);

    // Variables needed to manage the periodic thread.
    std::mutex cv_m;
    std::condition_variable cv;
    int watchIntervalSec = 0;
    bool watcherStarted = false;
    std::thread watcher_thread;

public:
    custSampleLoader();
    ~custSampleLoader();

    // Virtual functions of the base class defined here
    CustomLoaderStatus loaderInit(const std::string& loader_path);
    CustomLoaderStatus loaderDeInit();
    CustomLoaderStatus unloadModel(const std::string& modelName, int version);
    CustomLoaderStatus loadModel(const std::string& modelName, const std::string& basePath, const int version,
        const std::string& loaderOptions, char** xmlBuffer, int* xmlLen, char** binBuffer,
        int* binLen);
    CustomLoaderStatus getModelBlacklistStatus(const std::string& modelName, int version);

    // Sample loader specific variables
    // Thread function and helper to periodically check model status
    void threadFunction();
    void checkModelStatus();

    //Functions to start and stop the periodic thread.
    void startWatcher(int intervalSec);
    void watcherJoin();
};

extern "C" CustomLoaderInterface* createCustomLoader() {
    return new custSampleLoader();
}

custSampleLoader::custSampleLoader() {
    std::cout << "custSampleLoader: Instance of Custom SampleLoader created" << std::endl;
}

custSampleLoader::~custSampleLoader() {
    std::cout << "custSampleLoader: Instance of Custom SampleLoader deleted" << std::endl;
    if (watcherStarted == true)
        watcherJoin();
}

CustomLoaderStatus custSampleLoader::loaderInit(const std::string& loader_path) {
    std::cout << "custSampleLoader: Custom loaderInit" << loader_path << std::endl;
    return CustomLoaderStatus::OK;
}

// Helper function to load the binary files
int custSampleLoader::load_files(std::string& binFile, std::string& modelFile, int modelType, 
		char** modelBuffer, char** binBuffer, int* modelLen, int* binLen) {

    std::streampos size;

    // incase the model is a onxx or blob type, the bin file will not be there.
    // skip parsing the bin file and return NULL in binBuffer
    if (modelType == SAMPLE_LOADER_IR_MODEL){
	    std::ifstream bfile(binFile, std::ios::in | std::ios::binary | std::ios::ate);
	    if (bfile.is_open()) {
		    size = bfile.tellg();
		    *binLen = size;
		    *binBuffer = new char[size];
		    bfile.seekg(0, std::ios::beg);
		    bfile.read(*binBuffer, size);
		    bfile.close();
	    } 
	    else {
		    std::cout << "Unable to open bin file" << std::endl;
		    return SAMPLE_LOADER_ERROR;
	    }
    }
    else {
	    *binBuffer = NULL;
	    binLen = 0;
    }

    std::ifstream xfile(modelFile, std::ios::in | std::ios::ate);
    if (xfile.is_open()) {
        size = xfile.tellg();
        *modelLen = size;
        *modelBuffer = new char[size];
        xfile.seekg(0, std::ios::beg);
        xfile.read(*modelBuffer, size);
        xfile.close();
    } else {
        std::cout << "Unable to open xml file" << std::endl;
        return SAMPLE_LOADER_ERROR;
    }
    return SAMPLE_LOADER_OK;
}

int custSampleLoader::extract_input_params(const std::string& basePath, const int version, const std::string& loaderOptions,
		std::string& binFile, std::string& modelFile, std::string& enableFile, int& modelType){

    int ret = SAMPLE_LOADER_OK;
    Document doc;

    if (basePath.empty()| loaderOptions.empty()) {
        std::cout << "custSampleLoader: Invalid input parameters to loadModel" << std::endl;
        return SAMPLE_LOADER_ERROR;
    }
    
    std::string fullPath = basePath + "/" + std::to_string(version);

    // parse jason input string
    if (doc.Parse(loaderOptions.c_str()).HasParseError()) {
        return SAMPLE_LOADER_ERROR;
    }

    for (Value::ConstMemberIterator itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr)
        printf("Type of member %s is %s\n", itr->name.GetString(), itr->value.GetString());

    // Optional Enable file
    if (doc.HasMember("enable_file")) {
	enableFile = fullPath + "/" + doc["enable_file"].GetString();
        std::cout << "Enable File = " << enableFile << std::endl;
    }

    // Xml file is sent . So need to have bin file. Get both
    if (doc.HasMember("model_file")) {
	std::string modelName =  doc["model_file"].GetString();
	modelFile = fullPath + "/" + modelName;
        std::cout << "modelFile:" << modelFile << std::endl;

	std::string extn;
	extn = modelName.substr(modelName.find_last_of(".")+1);
	if (extn == "xml"){
		std::cout << "XML File" << std::endl;
		modelType = SAMPLE_LOADER_IR_MODEL;
	}
	else if (extn == "onxx"){
		std::cout << "XML File" << std::endl;
		modelType = SAMPLE_LOADER_ONNX_MODEL;
	}
	else if (extn == "blob"){
		std::cout << "XML File" << std::endl;
		modelType = SAMPLE_LOADER_BLOB_MODEL;
	}
	else {
		std::cout << "UNKNOWN file extension" << std::endl;
		return SAMPLE_LOADER_ERROR;
	}
    }

    if (modelType == SAMPLE_LOADER_IR_MODEL) {
	    if (doc.HasMember("bin_file")) {
		    binFile = fullPath + "/" + doc["bin_file"].GetString();
		    std::cout << "Bin File = " << binFile << std::endl;
	    }
    }

    return ret;
}

void custSampleLoader::threadFunction() {
    std::cout << "custSampleLoader: Thread Start" << std::endl;
    bool waitContinue = true;
    std::unique_lock<std::mutex> lk(cv_m);

    while (watcherStarted != true) {
        // wait for the watcher to be started fully
    }

    while (waitContinue) {
        std::cout << "custSampleLoader: Doing Some Work " << std::endl;
	std::cv_status ret = cv.wait_for(lk, std::chrono::seconds(watchIntervalSec));
	if (ret == std::cv_status::timeout) {
		// before starting next wait period, check if someone trying to disable the thread.
		if (watcherStarted == false)
			break;
		// Now check status of all the models and create a new blacklist
		std::cout << "Checking Model Status" << std::endl;
		checkModelStatus();	
	}
	else {
		std::cout << "Signalled to stop.. exiting..." <<std::endl;
		waitContinue = false;
	}
    }
    std::cout << "custSampleLoader: Thread END" << std::endl;
}

void custSampleLoader::checkModelStatus() {
    std::map<model_id_t,bool> models_blacklist_local;
    
    for (auto it= std::begin(models_loaded); it != std::end(models_loaded); it++){
	if (models_enable.find(*it) == models_enable.end())
		continue;

         std::string fileName = models_enable[*it];
	 std::cout << "Reading File:: " << fileName << std::endl;
	 std::ifstream fileToRead(fileName);
	 std::string stateStr;
	 if (fileToRead.is_open()) {
              getline(fileToRead, stateStr);
         }

         if (stateStr == "DISABLED") {
		std::cout << "Balcklisting Model:: " << it->first << std::endl;
		models_blacklist_local.insert({*it,true});
	 }
    }

    // Now take the mutex and copy to original map
    std::lock_guard<std::mutex> guard(map_mutex);
    models_blacklist.clear();
    models_blacklist.insert(models_blacklist_local.begin(),models_blacklist_local.end());
}

void custSampleLoader::startWatcher(int interval) {
    watchIntervalSec = interval;

    if ((!watcherStarted) && (watchIntervalSec > 0)) {
	std::thread th(std::thread(&custSampleLoader::threadFunction, this));
        watcher_thread = std::move(th);
        watcherStarted = true;
    }
    std::cout << "custSampleLoader: StartWatcher" << std::endl;
}

void custSampleLoader::watcherJoin() {
    std::cout << "custSampleLoader: watcherJoin()" << std::endl;
    if (watcherStarted) {
	    if (watcher_thread.joinable()) {
		    watcherStarted = false;
		    cv.notify_all();
		    watcher_thread.join();
	    }
    }
}

/* 
 * From the custom loader options extract the mdeol file name and other neede information and
 * load the model and optional bin file into buffers and return
 */
CustomLoaderStatus custSampleLoader::loadModel(const std::string& modelName, const std::string& basePath, const int version,
		const std::string& loaderOptions, char** xmlBuffer, int* xmlLen,
		char** binBuffer, int* binLen) {
	std::cout << "custSampleLoader: Custom loadModel" << std::endl;

	std::string binFile;
	std::string modelFile;
	std::string enableFile;
	int modelType = SAMPLE_LOADER_IR_MODEL;
	CustomLoaderStatus st = CustomLoaderStatus::MODEL_LOAD_ERROR;

	int ret =
		extract_input_params(basePath, version, loaderOptions, binFile, modelFile, enableFile, modelType);
	if (ret != SAMPLE_LOADER_OK){
		std::cout << "custSampleLoader: Invalid custom loader options" << std::endl;
		return st;
	}

	// load models
	ret = load_files(binFile, modelFile, modelType, xmlBuffer, binBuffer, xmlLen, binLen);
	if (ret != SAMPLE_LOADER_OK || *xmlBuffer == NULL){
		std::cout << "custSampleLoader: Could not read model files" << std::endl;
		return CustomLoaderStatus::INTERNAL_ERROR;
	}

	/* Start the watcher thread after first moel load */
	if (watcherStarted == false) {
		startWatcher(MODEL_CHECK_PERIOD);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	models_loaded.emplace_back (std::make_pair(modelName, version));

        if (!(enableFile.empty())){
	     models_enable.insert({std::make_pair(modelName, version),enableFile});
	}
	if (modelType == SAMPLE_LOADER_IR_MODEL)
		st = CustomLoaderStatus::MODEL_TYPE_IR;
	else if (modelType == SAMPLE_LOADER_ONNX_MODEL)
		st = CustomLoaderStatus::MODEL_TYPE_ONNX;
	else if (modelType == SAMPLE_LOADER_BLOB_MODEL)
		st = CustomLoaderStatus::MODEL_TYPE_BLOB;

	return st;
}

// Unload model from loaded models list.
CustomLoaderStatus custSampleLoader::unloadModel(const std::string& modelName, int version) {
	std::cout << "custSampleLoader: Custom unloadModel" << std::endl;

    model_id_t toFind = std::make_pair(modelName, version);

    auto it = models_loaded.begin();
    for (; it != models_loaded.end(); it++) {
        if (*it == toFind)
            break;
    }

    if (it == models_loaded.end()) {
        std::cout << modelName << " is not loaded" << std::endl;
    } else {
        models_loaded.erase(it);
    }
    return CustomLoaderStatus::OK;
}

CustomLoaderStatus custSampleLoader::loaderDeInit() {
    std::cout << "custSampleLoader: Custom loaderDeInit" << std::endl;
    if (watcherStarted == true)
        watcherJoin();
    return CustomLoaderStatus::OK;
}

CustomLoaderStatus custSampleLoader::getModelBlacklistStatus(const std::string& modelName, int version) {

    std::cout << "custSampleLoader: Custom getModelBlacklistStatus" << std::endl;

    model_id_t toFind = std::make_pair(modelName, version);

    std::lock_guard<std::mutex> guard(map_mutex);
    if (models_blacklist.size() == 0)
        return CustomLoaderStatus::OK;

    auto it = models_blacklist.find(toFind);
    if (it == models_blacklist.end()) {
        return CustomLoaderStatus::OK;
    }

    /* model name and version in blacklist.. return true */
    return CustomLoaderStatus::MODEL_BLACKLISTED;
}

