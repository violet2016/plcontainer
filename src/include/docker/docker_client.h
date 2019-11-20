#ifndef __DOCKER_CLIENT_H__
#define __DOCKER_CLIENT_H__

#define RAPIDJSON_HAS_STDSTRING 1
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <curl/curl.h>
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"

#define JSON_DOC rapidjson::Document
#define JSON_VAL rapidjson::Value

typedef enum{
    GET,
    POST,
    DELETE,
    PUT
} Method;

std::string param( const std::string& param_name, const std::string& param_value);
std::string param( const std::string& param_name, const char* param_value);
std::string param( const std::string& param_name, bool param_value);
std::string param( const std::string& param_name, int param_value);
std::string param( const std::string& param_name, JSON_DOC& param_value);

std::string jsonToString(JSON_VAL & doc);

class Docker{
    public :
        Docker();
        explicit Docker(std::string host);
        ~Docker();

        /*
        * System
        */
        JSON_DOC system_info();
        JSON_DOC docker_version();

        /*
        * Images
        */
        JSON_DOC list_images();

        /*
        * Containers
        */
        JSON_DOC list_containers(JSON_DOC& filters, bool all=false, int limit=-1, const std::string& since="", const std::string& before="", int size=-1);
        JSON_DOC inspect_containers(const std::vector<std::string>& container_ids);
        JSON_DOC create_container(JSON_DOC& parameters);
        JSON_DOC start_container(const std::string& container_id);
        JSON_DOC get_container_changes(const std::string& container_id);
        JSON_DOC stop_container(const std::string& container_id, int delay=-1);
        JSON_DOC kill_container(const std::string& container_id, int signal=-1);
        JSON_DOC kill_containers(std::vector<const std::string&> container_ids, int signal=-1);
        JSON_DOC pause_container(const std::string& container_id);
        JSON_DOC wait_container(const std::string& container_id);
        JSON_DOC delete_containers(const std::vector<std::string>& container_ids, bool v=true, bool force=true);

    private:
        std::string host_uri;
        bool is_multi;
        bool is_remote;
        CURLM *curlm{};
        CURLcode res{};
        int mActiveTransfers;

        JSON_DOC requestAndParse(Method method, const std::vector<std::string>& paths, JSON_DOC& param, long success_code = 200);
        JSON_DOC requestAndParseJson(Method method, const std::vector<std::string>& path, JSON_DOC& param, long success_code = 200);
        int multiCurlRequests(std::string& readBuffer, const std::vector<std::string>& paths, JSON_DOC& param, std::string method_str);
        static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp){
            ((std::string*)userp)->append((char*)contents, size * nmemb);
            return size * nmemb;
        }
};
#endif //__DOCKER_CLIENT_H__