/**
 * @created:	   		2012/05/16
 *
 * @file
 *
 * @author  			wei yao <yaocoder@gmail.com>
 *
 * @version 			1.0
 *
 * @LICENSE
 *
 * @brief				通用工具方法
 *
 */
#ifndef UTILS_H__
#define UTILS_H__

#include <netinet/in.h>
#include <unistd.h>
#include <sys/time.h>
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <boost/filesystem.hpp>
#include <string>
using namespace std;

namespace utils
{

static inline bool GetCurrentPath(std::string& current_path)
{
	try
	{
		boost::filesystem::path path = boost::filesystem::current_path();
		current_path = path.string();
		return true;
	}
	catch (boost::filesystem::filesystem_error & e)
	{
		cout << "current_path : " << current_path << ", error description :" << e.what() << endl;
		return false;
	}
}

static inline void SplitData(const std::string& str, const std::string& delimiter, std::vector<std::string>& vec_data)
{
	std::string s = str;
	size_t pos = 0;
	std::string token;
	while ((pos = s.find(delimiter)) != std::string::npos)
	{
		token = s.substr(0, pos);
		vec_data.push_back(token);
		s.erase(0, pos + delimiter.length());
	}
}

static inline bool FindCRLF(const std::string& s)
{
	if(s.find("\r\n") != std::string::npos)
		return true;
	else
		return false;
}

static inline std::string int2str(int v)
{
	std::stringstream ss;
	ss << v;
	return ss.str();
}

static inline int GetNumFromStr(string s, int pos, int n)
{
    string subs = s.substr(pos, n);
    return stoi(subs);
}

static inline string TransToByteStr(int s_num, int n)
{
    string s = to_string(s_num);
    int left_n = n - s.size();
    string front_0;
    while(left_n--)
        front_0 += '0';
    return front_0 + s;
}

template<class T>
class Singleton: private T
{
public:
	static T &Instance()
	{
		static Singleton<T> _instance;
		return _instance;
	}
private:
	Singleton()
	{
	}
	~Singleton()
	{
	}
};

template<typename T>
T& G()
{
	return Singleton<T>::Instance();
}

template<typename T> inline void SafeDeleteArray(T*& p)
{
	if (NULL != p)
	{
		delete[] p;
		p = 0;
	}
}

template<typename T> inline void SafeDelete(T*& p)
{
	if (NULL != p)
	{
		delete p;
		p = 0;
	}
}

}
#endif
