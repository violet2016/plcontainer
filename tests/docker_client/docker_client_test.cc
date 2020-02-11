#include "docker/docker_client.h"

int main() {
    JSON_DOC doc;
    Docker client = Docker();
    std::vector<std::string> container_ids;
    JSON_DOC param(rapidjson::kObjectType);
    JSON_VAL commands(rapidjson::kArrayType);
    commands.PushBack("/bin/bash", param.GetAllocator());
    param.AddMember("Cmd", commands, param.GetAllocator());
    param.AddMember("Image", "centos:7", param.GetAllocator());
    doc = client.create_container(param);
    std::cout << jsonToString(doc) << std::endl;
    std::string container_id1 = doc["data"]["Id"].GetString();
    container_ids.push_back(container_id1);
    /* start */
    JSON_DOC start_res;
    start_res = client.start_container(container_id1);
    if (start_res["data"].IsString() && start_res["data"].GetStringLength() == 0) {
        std::cout << "success! start res: " <<jsonToString(start_res) << std::endl;
    } else {
        std::cout << "failed! start res: " <<jsonToString(start_res) << std::endl;
    }

    JSON_DOC docker2_param(rapidjson::kObjectType);
    docker2_param.AddMember("Image", "hello-world:latest", docker2_param.GetAllocator());
    doc = client.create_container(docker2_param);
    std::cout << jsonToString(doc) << std::endl;

    /* delete containers */

    std::string container_id2 = doc["data"]["Id"].GetString();
    JSON_DOC filter = JSON_DOC();
    doc = client.list_containers(filter);
    std::cout << "list" << jsonToString(doc) << std::endl;
    container_ids.push_back(container_id2);
    doc = client.stat_containers(container_ids);
    JSON_DOC stat;
    std::string stat_string = doc["data"].GetString();
    std::string delimiter = "\n";

    size_t pos = 0;
    std::string token;
    while ((pos = stat_string.find(delimiter)) != std::string::npos) {
        token = stat_string.substr(0, pos);
        stat.Parse(token);
        std::cout << "stat is " << jsonToString(stat["memory_stats"]) << std::endl;
        stat_string.erase(0, pos + delimiter.length());
    }

    std::cout << "delete "<< container_ids.size()<< " containers" << std::endl;
    JSON_DOC delete_res;
    delete_res = client.delete_containers(container_ids);
    std::cout << "delete res: " <<jsonToString(delete_res) << std::endl;
    delete_res = client.delete_containers(container_ids);
    std::cout << "delete res: " <<jsonToString(delete_res) << std::endl;
    if (!delete_res["data"].IsNull()) {
        if (delete_res["data"].IsString()) {
            std::cout<< "is string" <<std::endl;
        }
    }
    return 0;
}