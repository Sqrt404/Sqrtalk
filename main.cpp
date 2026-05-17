#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>
#include <string>
#include <chrono>
#include <cstdlib>
#include <regex>
#include <array>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/locale.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <set>
#include <map>
#include <mutex>
#include <stdexcept>
#include <filesystem>
#include <unordered_map>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>
    #pragma comment(lib, "ws2_32.lib")
    #define SHUT_RDWR SD_BOTH
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/ioctl.h>
#endif

inline std::tm* safe_localtime(const std::time_t* time, std::tm* result) {
#ifdef _WIN32
    localtime_s(result, time);
    return result;
#else
    return localtime_r(time, result);
#endif
}

inline void safe_close(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

#include <queue>
#include <deque>
#include <functional>

using json = nlohmann::json;
using websocketpp::connection_hdl;
namespace fs = std::filesystem;

std::string charset = "0123456789abcdefghijklmnopqrstuvwxyz";

struct ServerConfig {

    uint16_t ws_port = 9002;
    uint16_t file_server_port = 3467;
    size_t thread_pool_size = 4;
    size_t max_http_body_size = 65536;
    int ws_open_timeout_ms = 5000;
    int ws_close_timeout_ms = 5000;
    int ping_interval_sec = 10;

    std::string password_salt = "SqrtalkDefaultSalt2024!@#$";

    int rate_limit_msg_interval_ms = 300;
    int rate_limit_msg_burst = 30;
    int rate_limit_msg_window_sec = 60;
    int rate_limit_login_interval_sec = 5;
    int max_registrations_per_ip = 5;

    size_t min_username_len = 3;
    size_t max_username_len = 20;
    size_t max_message_len = 256;
    size_t guest_name_len = 3;

    std::string files_dir = "files";
    std::string logs_dir = "logs";
    std::string users_file = "users.json";
    std::string permission_groups_file = "permission_groups.json";
    std::string banned_users_file = "banned_users.json";
    std::string files_meta_file = "files.json";

    bool load(const std::string& path = "config.json");
};

extern ServerConfig config;

class Logger {
	public:
		enum class Level {
		    INFO, WARNING, ERROR
		};

		Logger() : running(true), worker(&Logger::log_worker, this) {}
		~Logger() {
			running = false;
			cv.notify_all();
			if (worker.joinable()) {
				worker.join();
			}
		}

		void log(Level level, const std::string& message) {
			std::lock_guard<std::mutex> lock(queue_mutex);
			log_queue.push({level, message});
			cv.notify_one();
		}

	private:
		struct LogEntry {
			Level level;
			std::string message;
		};

		std::atomic<bool> running;
		std::mutex queue_mutex;
		std::condition_variable cv;
		std::queue<LogEntry> log_queue;
		std::thread worker;

		void log_worker() {
			while (running || !log_queue.empty()) {
				std::unique_lock<std::mutex> lock(queue_mutex);
				cv.wait(lock, [&] { return !log_queue.empty() || !running; });

				while (!log_queue.empty()) {
					auto entry = log_queue.front();
					log_queue.pop();
					lock.unlock();

					auto now = std::chrono::system_clock::now();
					auto t = std::chrono::system_clock::to_time_t(now);
					std::tm tm_buf;
					safe_localtime(&t, &tm_buf);
					std::stringstream ss;
					ss << std::put_time(&tm_buf, "%F %T ")
					   << level_to_string(entry.level) << ": "
					   << entry.message;
					std::string log_line = ss.str();
					std::cerr << log_line << std::endl;

					std::stringstream file_ss;
					file_ss << std::put_time(&tm_buf, "%Y-%m");
					std::string log_dir = "logs";
					if (!fs::exists(log_dir)) {
						fs::create_directory(log_dir);
					}
					std::ofstream log_file(log_dir + "/[" + file_ss.str() + "].log", std::ios_base::app);
					if (log_file.is_open()) {
						log_file << log_line << std::endl;
					}

					lock.lock();
				}
			}
		}

		static std::string level_to_string(Level level) {
			switch(level) {
				case Level::INFO:
					return "INFO";
				case Level::WARNING:
					return "WARNING";
				case Level::ERROR:
					return "ERROR";
				default:
					return "UNKNOWN";
			}
		}
};

Logger logger;

bool ServerConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        logger.log(Logger::Level::WARNING, "Config file not found at " + path + ", using defaults");
        return false;
    }
    try {
        json j;
        file >> j;

        auto read_val = [&](const json& obj, const std::string& key, auto& field) -> void {
            if (obj.contains(key)) {
                field = obj[key].get<std::decay_t<decltype(field)>>();
            }
        };

        if (j.contains("server")) {
            auto& s = j["server"];
            read_val(s, "ws_port", ws_port);
            read_val(s, "file_server_port", file_server_port);
            read_val(s, "thread_pool_size", thread_pool_size);
            read_val(s, "max_http_body_size", max_http_body_size);
            read_val(s, "ws_open_timeout_ms", ws_open_timeout_ms);
            read_val(s, "ws_close_timeout_ms", ws_close_timeout_ms);
            read_val(s, "ping_interval_sec", ping_interval_sec);
        }
        if (j.contains("password")) {
            auto& p = j["password"];
            read_val(p, "salt", password_salt);
        }
        if (j.contains("rate_limit")) {
            auto& r = j["rate_limit"];
            read_val(r, "msg_interval_ms", rate_limit_msg_interval_ms);
            read_val(r, "msg_burst", rate_limit_msg_burst);
            read_val(r, "msg_window_sec", rate_limit_msg_window_sec);
            read_val(r, "login_interval_sec", rate_limit_login_interval_sec);
            read_val(r, "max_registrations_per_ip", max_registrations_per_ip);
        }
        if (j.contains("validation")) {
            auto& v = j["validation"];
            read_val(v, "min_username_len", min_username_len);
            read_val(v, "max_username_len", max_username_len);
            read_val(v, "max_message_len", max_message_len);
            read_val(v, "guest_name_len", guest_name_len);
        }
        if (j.contains("paths")) {
            auto& p = j["paths"];
            read_val(p, "files_dir", files_dir);
            read_val(p, "logs_dir", logs_dir);
            read_val(p, "users_file", users_file);
            read_val(p, "permission_groups_file", permission_groups_file);
            read_val(p, "banned_users_file", banned_users_file);
            read_val(p, "files_meta_file", files_meta_file);
        }

        logger.log(Logger::Level::INFO, "Configuration loaded from " + path);
        return true;
    } catch (const std::exception& e) {
        logger.log(Logger::Level::ERROR, std::string("Failed to parse config.json: ") + e.what());
        return false;
    }
}

ServerConfig config;

class SocketException : public std::runtime_error {
	public:
		SocketException(const std::string& message, int error_code)
			: std::runtime_error(message + " Error code: " + std::to_string(error_code)),
			  error_code_(error_code) {
			logger.log(Logger::Level::ERROR, what());
		}

		int error_code() const {
			return error_code_;
		}

	private:
		int error_code_;
};

class Socket {
	public:
		enum class Protocol {
		    TCP, UDP
		};

		static void Initialize() {
	#ifdef _WIN32
					WSADATA wsaData;
					if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
						throw std::runtime_error("WSAStartup failed");
					}
	#endif
		}

		Socket() : sock_(-1) {}

		explicit Socket(int s) : sock_(s) {
			int flag = 1;
				setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
		}

		~Socket() {
			Close();
		}

		void Create(Protocol protocol) {
			int type = (protocol == Protocol::TCP) ? SOCK_STREAM : SOCK_DGRAM;
			int proto = (protocol == Protocol::TCP) ? IPPROTO_TCP : IPPROTO_UDP;

			sock_ = socket(AF_INET, type, proto);
			if (sock_ < 0) {
				throw SocketException("Socket creation failed", errno);
			}
		}

		void Bind(const std::string& address, unsigned short port) {
			sockaddr_in service = CreateAddress(address, port);
			if (bind(sock_, (sockaddr*)&service, sizeof(service)) < 0) {
				throw SocketException("Bind failed", errno);
			}
		}

		void Listen(int backlog = SOMAXCONN) {
			if (listen(sock_, backlog) < 0) {
				throw SocketException("Listen failed", errno);
			}
		}

		Socket Accept() {
			int client_socket = accept(sock_, nullptr, nullptr);
			if (client_socket < 0) {
				throw SocketException("Accept failed", errno);
			}
			return Socket(client_socket);
		}

		size_t Send(const std::string& data) {
			ssize_t result = send(sock_, data.data(), data.size(), 0);
			if (result < 0) {
#ifdef _WIN32
				int err = WSAGetLastError();
				if (err == WSAECONNRESET || err == WSAECONNABORTED) {
					return 0;
				}
#else
				if (errno == EPIPE || errno == ECONNRESET) {
					return 0;
				}
#endif
				throw SocketException("Send failed", errno);
			}
			return static_cast<size_t>(result);
		}

		std::string Receive(size_t buffer_size = 4096) {
			std::string buffer(buffer_size, 0);
			ssize_t result = recv(sock_, &buffer[0], buffer_size, 0);
			if (result < 0) {
				throw SocketException("Receive failed", errno);
			}
			buffer.resize(result);
			return buffer;
		}

		void Close() {
			if (sock_ >= 0) {
				close(sock_);
				sock_ = -1;
			}
		}

		int get_socket() const {
			return sock_;
		}

		std::string get_peer_address() const {
			sockaddr_in addr {};
			socklen_t addr_len = sizeof(addr);
			if (getpeername(sock_, (sockaddr*)&addr, &addr_len) == 0) {
				char ip[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
				return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
			}
			return "unknown";
		}

		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;

		Socket(Socket&& other) noexcept :
			sock_(other.sock_) {
			other.sock_ = -1;
		}

		Socket& operator=(Socket&& other) noexcept {
			if (this != &other) {
				Close();
				sock_ = other.sock_;
				other.sock_ = -1;
			}
			return *this;
		}

	private:
		int sock_;

		sockaddr_in CreateAddress(const std::string& address, unsigned short port) {
			sockaddr_in service {};
			service.sin_family = AF_INET;
			service.sin_port = htons(port);
			if (address.empty()) {
				service.sin_addr.s_addr = INADDR_ANY;
			} else {
				inet_pton(AF_INET, address.c_str(), &service.sin_addr);
			}
			return service;
		}
};

class ThreadPool {
	public:
		explicit ThreadPool(size_t threads) : stop(false) {
			for (size_t i = 0; i < threads; ++i)
				workers.emplace_back([this] {
				for (;;) {
					std::unique_ptr<TaskBase> task;
					{
						std::unique_lock<std::mutex> lock(this->queue_mutex);
						this->condition.wait(lock,
						[this] { return this->stop || !this->tasks.empty(); });
						if (this->stop && this->tasks.empty())
							return;
						task = std::move(this->tasks.front());
						this->tasks.pop_front();
					}
					task->execute();
				}
			});
		}

		template<class F>
		void enqueue(F&& f) {
			using TaskType = Task<F>;
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				tasks.emplace_back(new TaskType(std::forward<F>(f)));
			}
			condition.notify_one();
		}

		~ThreadPool() {
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				stop = true;
			}
			condition.notify_all();
			for (std::thread &worker : workers)
				worker.join();
		}

	private:
		struct TaskBase {
			virtual void execute() = 0;
			virtual ~TaskBase() = default;
		};

		template<class F>
		struct Task : TaskBase {
			F f;
			Task(F&& f_) : f(std::forward<F>(f_)) {}
			void execute() override {
				f();
			}
		};

		std::vector<std::thread> workers;
		std::deque<std::unique_ptr<TaskBase>> tasks;

		std::mutex queue_mutex;
		std::condition_variable condition;
		bool stop;
};

class ChatServer {
	public:
		ChatServer();
		void run(uint16_t port);
		void FileServer();

	private:
		typedef websocketpp::server<websocketpp::config::asio> server;
		server m_server;
		std::set<connection_hdl, std::owner_less<connection_hdl>> m_connections;
		struct FileInfo {
			std::string master = "";
			std::string FileName = "";
			std::string NickName = "";
			bool IsPrivate = false;
		};
		struct UserInfo {
			std::string password;
			int level;
			bool chat = true;
			std::string permission_group;
			std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> channel;
			std::chrono::steady_clock::time_point last_login_time;
			std::chrono::steady_clock::time_point last_message_time;
			std::chrono::steady_clock::time_point MessageTimer;
			int message_count = 0;
			std::set<std::string> ignore_users;
			std::set<std::string> UserFile;
		};
		struct PermissionGroup {
			std::string tag, color;
			int level;
			std::set<std::string> permissions;
		};
		struct ChannelInfo {
			std::multiset<std::string> list;
			bool IsLock = false;
		};
		struct TokenInfo {
			std::string username = "";
			std::string time = "";
			std::string IP = "";
		};
		bool IsLockSite = false;
		sqlite3* m_db = nullptr;
		std::map<std::string, std::queue<std::string>> leave_message;
		std::map<std::string, std::set<std::string>> m_IPs;
		std::map<std::string, FileInfo> m_files;
		std::map<std::string, ChannelInfo> channel;
		std::map<std::string, TokenInfo> tokens;
		std::map<std::string, UserInfo> m_users;
		std::map<std::string, PermissionGroup> m_permission_groups;
		std::map<std::string, std::string> m_banned_users;
		std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> m_connection_to_username;
		std::shared_ptr<boost::asio::steady_timer> m_timer;
		std::mutex m_mutex;

		void on_open(connection_hdl hdl);
		void on_close(connection_hdl hdl);
		void on_message(connection_hdl hdl, server::message_ptr msg);
		void on_fail(connection_hdl hdl);
		void send_ping();
		void log(const std::string &message);
		std::string GenerateToken(std::string username);
		bool RemoveToken(std::string Token);
		void handle_login(const json &j, connection_hdl hdl);
		void handle_guest(const json &j, connection_hdl hdl);
		void handle_file_upload(const json &j, connection_hdl hdl);
		void handle_message(const json &j, connection_hdl hdl);
		std::string get_username_by_connection(connection_hdl hdl);
		bool is_valid_username(const std::string &username);
		bool is_valid_password(const std::string &password);
		bool is_valid_message(const std::string &message);
		bool is_logged_in(connection_hdl hdl);
		int level(connection_hdl hdl);
		void send_response(connection_hdl hdl, const std::string &type, const std::string &message);
		void broadcast_message(const json &message, const std::string &name, const std::string &channel, connection_hdl hdl);
		void load_users();
		void save_users();
		void load_permission_groups();
		void save_permission_groups();
		void load_banned_users();
		void save_banned_users();
		void load_files();
		void save_files();
		void handle_request(Socket client, const fs::path& web_root);
		std::string get_IP_from_hdl(connection_hdl hdl);
		std::string extractIPv4(const std::string& address);
		bool check_rate_limit(connection_hdl hdl, bool type);
		bool validate_command_args(const std::vector<std::string>& parts, size_t min_args,
		                           connection_hdl hdl, const std::string& usage);

		void send_online_user_list(connection_hdl hdl, const std::string& username);
		void notify_channel_join(const std::string& username, const std::string& channel_name);
		void notify_channel_leave(const std::string& username, const std::string& channel_name, connection_hdl hdl);
		bool check_banned_user(const std::string& username, connection_hdl hdl);
		bool check_banned_ip(connection_hdl hdl);
		bool check_lock_restriction(const std::string& channel_name, connection_hdl hdl,
		                            const std::string& username = "");
		bool has_permission(connection_hdl hdl, const std::string& perm);
		void init_database();
		void track_login_ip(const std::string& username, const std::string& ip);
		std::string get_login_ips(const std::string& username, int limit = 5);
		void save_json_file(const std::string& filepath, const json& j,
		                    const std::string& empty_content = "{}");
		std::string hash_password(const std::string& password, const std::string& salt = "");
		std::string generate_salt();
};

std::string wstring_to_utf8(const std::wstring& wstr) {
	return boost::locale::conv::utf_to_utf<char>(wstr);
}

std::string base64_encode(const std::string &input) {
	const size_t encoded_size = boost::beast::detail::base64::encoded_size(input.size());
	std::string output;
	output.resize(encoded_size);
	auto const result = boost::beast::detail::base64::encode(
	                        output.data(), input.data(), input.size());
	output.resize(result);
	return output;
}

std::string base64_decode(const std::string &input) {
	std::string output;
	output.resize(boost::beast::detail::base64::decoded_size(input.size()));
	auto const result = boost::beast::detail::base64::decode(
	                        &output[0], input.data(), input.size());
	output.resize(result.first);
	return output;
}

class SHA256 {
	public:
		SHA256() {
			reset();
		}

		void update(const uint8_t* data, size_t length) {
			for (size_t i = 0; i < length; ++i) {
				data_[datalen_++] = data[i];
				if (datalen_ == 64) {
					transform();
					bitlen_ += 512;
					datalen_ = 0;
				}
			}
		}

		void update(const std::string& data) {
			update(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
		}

		std::string final() {
			uint8_t hash[32];
			pad();
			revert(hash);
			std::stringstream ss;
			for (int i = 0; i < 32; ++i) {
				ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
			}
			return ss.str();
		}

	private:
		uint8_t data_[64];
		uint32_t datalen_;
		uint64_t bitlen_;
		uint32_t state_[8];

		static constexpr std::array<uint32_t, 64> k_ = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
			0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
			0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
			0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
			0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
			0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
			0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
			0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
			0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
		};

		static uint32_t rotr(uint32_t x, uint32_t n) {
			return (x >> n) | (x << (32 - n));
		}

		static uint32_t choose(uint32_t e, uint32_t f, uint32_t g) {
			return (e & f) ^ (~e & g);
		}

		static uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
			return (a & b) ^ (a & c) ^ (b & c);
		}

		static uint32_t sig0(uint32_t x) {
			return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
		}

		static uint32_t sig1(uint32_t x) {
			return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
		}

		void transform() {
			uint32_t maj, xorA, ch, xorE, sum, newA, newE, m[64];
			uint32_t state[8];

			for (int i = 0, j = 0; i < 16; ++i, j += 4)
				m[i] = (data_[j] << 24) | (data_[j + 1] << 16) | (data_[j + 2] << 8) | (data_[j + 3]);
			for (int k = 16; k < 64; ++k)
				m[k] = sig1(m[k - 2]) + m[k - 7] + sig0(m[k - 15]) + m[k - 16];

			for (int i = 0; i < 8; ++i)
				state[i] = state_[i];

			for (int i = 0; i < 64; ++i) {
				maj = majority(state[0], state[1], state[2]);
				xorA = rotr(state[0], 2) ^ rotr(state[0], 13) ^ rotr(state[0], 22);

				ch = choose(state[4], state[5], state[6]);

				xorE = rotr(state[4], 6) ^ rotr(state[4], 11) ^ rotr(state[4], 25);

				sum = m[i] + k_[i] + state[7] + ch + xorE;
				newA = xorA + maj + sum;
				newE = state[3] + sum;

				state[7] = state[6];
				state[6] = state[5];
				state[5] = state[4];
				state[4] = newE;
				state[3] = state[2];
				state[2] = state[1];
				state[1] = state[0];
				state[0] = newA;
			}

			for (int i = 0; i < 8; ++i)
				state_[i] += state[i];
		}

		void pad() {
			uint64_t i = datalen_;

			if (datalen_ < 56) {
				data_[i++] = 0x80;
				while (i < 56)
					data_[i++] = 0x00;
			} else {
				data_[i++] = 0x80;
				while (i < 64)
					data_[i++] = 0x00;
				transform();
				memset(data_, 0, 56);
			}

			bitlen_ += datalen_ * 8;
			data_[63] = bitlen_;
			data_[62] = bitlen_ >> 8;
			data_[61] = bitlen_ >> 16;
			data_[60] = bitlen_ >> 24;
			data_[59] = bitlen_ >> 32;
			data_[58] = bitlen_ >> 40;
			data_[57] = bitlen_ >> 48;
			data_[56] = bitlen_ >> 56;
			transform();
		}

		void revert(uint8_t* hash) {
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 8; ++j) {
					hash[i + (j * 4)] = (state_[j] >> (24 - i * 8)) & 0x000000ff;
				}
			}
		}

		void reset() {
			datalen_ = 0;
			bitlen_ = 0;
			state_[0] = 0x6a09e667;
			state_[1] = 0xbb67ae85;
			state_[2] = 0x3c6ef372;
			state_[3] = 0xa54ff53a;
			state_[4] = 0x510e527f;
			state_[5] = 0x9b05688c;
			state_[6] = 0x1f83d9ab;
			state_[7] = 0x5be0cd19;
		}
};

std::string sha256(const std::string &str) {
	SHA256 sha;
	sha.update(str);
	return sha.final();
}

uint64_t getCurrentTimestamp() {
	return static_cast<uint64_t>(std::time(nullptr)) / 180;
}

std::string getCurrentTime() {
	auto now = std::chrono::system_clock::now();
	std::time_t now_time = std::chrono::system_clock::to_time_t(now);
	std::tm tm_struct;
	safe_localtime(&now_time, &tm_struct);
	std::stringstream ss;
	ss << std::put_time(&tm_struct, "%Y-%m-%d %X");
	return ss.str();
}

std::string timestampToBytes(uint64_t timestamp) {
	std::string bytes;
	for (int i = 7; i >= 0; --i) {
		bytes.push_back(static_cast<char>((timestamp >> (8 * i)) & 0xFF));
	}
	return bytes;
}

std::string generateTOTP(const std::string& key, uint64_t timestamp) {
	std::string timeBytes = timestampToBytes(timestamp);
	std::string hmacResult = sha256(key + timeBytes);

	int offset = hmacResult[hmacResult.length() - 1] & 0xF;
	uint32_t binary = ((hmacResult[offset] & 0x7F) << 24 | (hmacResult[offset + 1] & 0xFF) << 16 | (hmacResult[offset + 2] & 0xFF) << 8 | (hmacResult[offset + 3] & 0xFF));

	uint32_t otp = binary % 100000000;
	std::ostringstream oss;
	oss << std::setw(6) << std::setfill('0') << otp;
	return oss.str();
}

std::string Get2FAPasscode(const std::string& password) {
	uint64_t timestamp = getCurrentTimestamp();
	return generateTOTP(password, timestamp);
}

bool startsWith(const std::string str, const std::string str1) {
	return str.find(str1) == 0;
}

std::vector<std::string> splitString(std::string s, int Num = -1) {
	std::vector<std::string> result;
	int num = 0;
	result.push_back("");
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == ' ') {
			if (Num == -1 || num < Num - 1) {
				num++;
				result.push_back("");
				continue;
			}
		}
		result[num] += s[i];
	}

	return result;
}

std::string GetRandomString(int Num) {
	static std::mt19937 rng(std::random_device{}());
	std::string result = "";
	while (Num--) {
		result += charset[rng() % charset.size()];
	}
	return result;
}

ChatServer::ChatServer() {
	m_server.init_asio();

	m_server.set_user_agent("Sqrtalk");
	m_server.set_max_http_body_size(config.max_http_body_size);

	m_server.set_open_handshake_timeout(config.ws_open_timeout_ms);
	m_server.set_close_handshake_timeout(config.ws_close_timeout_ms);

	m_server.clear_access_channels(websocketpp::log::alevel::all);
	m_server.clear_error_channels(websocketpp::log::elevel::all);

	m_server.set_open_handler(std::bind(&ChatServer::on_open, this, std::placeholders::_1));
	m_server.set_close_handler(std::bind(&ChatServer::on_close, this, std::placeholders::_1));
	m_server.set_message_handler(std::bind(&ChatServer::on_message, this, std::placeholders::_1, std::placeholders::_2));
	m_server.set_fail_handler(std::bind(&ChatServer::on_fail, this, std::placeholders::_1));
	init_database();
	load_banned_users();
	load_files();
}

void ChatServer::init_database() {
		std::string db_path = "sqrtalk.db";
		if (sqlite3_open(db_path.c_str(), &m_db) != SQLITE_OK) {
			log(std::string("Failed to open database: ") + sqlite3_errmsg(m_db));
			return;
		}
		log("Database opened: " + db_path);

		// Create tables
		const char* sql_users =
			"CREATE TABLE IF NOT EXISTS users ("
			"username TEXT PRIMARY KEY,"
			"password TEXT NOT NULL,"
			"level INTEGER DEFAULT 0,"
			"chat INTEGER DEFAULT 1,"
			"permission_group TEXT DEFAULT 'normal',"
			"ignore_users TEXT DEFAULT '[]',"
			"user_files TEXT DEFAULT '[]',"
			"registered_ip TEXT DEFAULT ''"
			")";
		const char* sql_groups =
			"CREATE TABLE IF NOT EXISTS permission_groups ("
			"name TEXT PRIMARY KEY,"
			"tag TEXT DEFAULT '',"
			"color TEXT DEFAULT '',"
			"level INTEGER DEFAULT 0,"
			"permissions TEXT DEFAULT '[]'"
			")";
		const char* sql_history =
			"CREATE TABLE IF NOT EXISTS login_history ("
			"username TEXT NOT NULL,"
			"ip TEXT NOT NULL,"
			"login_time TEXT NOT NULL"
			")";

		auto exec_sql = [&](const char* sql, const char* label) {
			char* err = nullptr;
			if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
				log(std::string("SQLite error [") + label + "]: " + err);
				sqlite3_free(err);
			}
		};
		exec_sql(sql_users, "create users");
		exec_sql(sql_groups, "create permission_groups");
		exec_sql(sql_history, "create login_history");

		// Migrate from JSON if tables are empty
		auto is_table_empty = [&](const std::string& table) -> bool {
			sqlite3_stmt* stmt = nullptr;
			std::string sql = "SELECT COUNT(*) FROM " + table;
			if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
				bool empty = true;
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					empty = (sqlite3_column_int(stmt, 0) == 0);
				}
				sqlite3_finalize(stmt);
				return empty;
			}
			sqlite3_finalize(stmt);
			return true;
		};

		bool migrate_users = is_table_empty("users") && fs::exists("users.json");
		bool migrate_groups = is_table_empty("permission_groups") && fs::exists("permission_groups.json");

		if (migrate_users) {
			log("Migrating users from JSON to SQLite...");
			std::ifstream file("users.json");
			if (file.is_open()) {
				try {
					json j;
					file >> j;
					if (j.contains("users")) {
						for (auto& [uname, info] : j["users"].items()) {
							std::string ignore = info.value("ignore_users", json::array()).dump();
							std::string ufiles = info.value("user_file", json::array()).dump();
							std::string reg_ip = info.value("registered_ip", "");
							std::string pwd = info["password"];
							int lvl = info["level"];
							int ch = info.value("chat", true) ? 1 : 0;
							std::string pms_group = info.value("permission_group", "normal");

							sqlite3_stmt* stmt = nullptr;
							const char* sql = "INSERT OR IGNORE INTO users VALUES(?,?,?,?,?,?,?,?)";
							if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
								sqlite3_bind_text(stmt, 1, uname.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 2, pwd.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_int(stmt, 3, lvl);
								sqlite3_bind_int(stmt, 4, ch);
								sqlite3_bind_text(stmt, 5, pms_group.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 6, ignore.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 7, ufiles.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 8, reg_ip.c_str(), -1, SQLITE_TRANSIENT);
								if (sqlite3_step(stmt) != SQLITE_DONE) {
									log(std::string("Failed to migrate user ") + uname + ": " + sqlite3_errmsg(m_db));
								}
								sqlite3_finalize(stmt);
							}
						}
					}
					if (j.contains("IPs")) {
						// Rebuild m_IPs from users table on load, no need to migrate here
					}
				} catch (const std::exception& e) {
					log(std::string("Migration error: ") + e.what());
				}
			}
		}

		if (migrate_groups) {
			log("Migrating permission groups from JSON to SQLite...");
			std::ifstream file("permission_groups.json");
			if (file.is_open()) {
				try {
					json j;
					file >> j;
					if (j.contains("permission_groups")) {
						for (auto& [name, info] : j["permission_groups"].items()) {
							std::string perms = info["allow"].dump();
							std::string tag = info.value("tag", "");
							std::string color = info.value("color", "");
							int lvl = info["level"];

							sqlite3_stmt* stmt = nullptr;
							const char* sql = "INSERT OR IGNORE INTO permission_groups VALUES(?,?,?,?,?)";
							if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
								sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_text(stmt, 3, color.c_str(), -1, SQLITE_TRANSIENT);
								sqlite3_bind_int(stmt, 4, lvl);
								sqlite3_bind_text(stmt, 5, perms.c_str(), -1, SQLITE_TRANSIENT);
								if (sqlite3_step(stmt) != SQLITE_DONE) {
									log(std::string("Failed to migrate group ") + name + ": " + sqlite3_errmsg(m_db));
								}
								sqlite3_finalize(stmt);
							}
						}
					}
				} catch (const std::exception& e) {
					log(std::string("Migration error: ") + e.what());
				}
			}
		}

		// Load data from SQLite into maps
		load_users();
		load_permission_groups();
	}

void ChatServer::run(uint16_t port) {
	m_server.set_reuse_addr(true);
	m_server.listen(boost::asio::ip::tcp::v4(), port);
	m_server.start_accept();
	m_timer = std::make_shared<boost::asio::steady_timer>(m_server.get_io_service());
	send_ping();
	log("Server run at " + getCurrentTime());

	while (true) {
		try {
			m_server.run();
			break;
		} catch (const std::exception &e) {
			log(std::string("Server run exception: ") + e.what());
			m_server.reset();
			m_server.init_asio();
			m_server.listen(boost::asio::ip::tcp::v4(), port);
			m_server.start_accept();
			log("Server restarted...");
		} catch (...) {
			log("Unknown server run exception.");
		}
	}
}

void ChatServer::on_open(connection_hdl hdl) {
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_connections.insert(hdl);
	}
	log("New connection opened from " + get_IP_from_hdl(hdl));
}

void ChatServer::on_close(connection_hdl hdl) {
	std::lock_guard<std::mutex> lock(m_mutex);

	std::string username = get_username_by_connection(hdl);
	if (username.empty()) {
		log("Anonymous connection closed, no username associated");
		m_connections.erase(hdl);
		return;
	}

	std::string channel_name = m_users[username].channel[hdl];
	if (!channel_name.empty()) {
		if (channel.find(channel_name) != channel.end()) {
			auto& user_list = channel[channel_name].list;
			auto it = user_list.find(username);
			if (it != user_list.end()) {
				user_list.erase(it);
				log(username + " removed from channel " + channel_name);
				notify_channel_leave(username, channel_name, hdl);
			}
		} else {
			log("Channel " + channel_name + " not found when closing connection");
		}
	}

	m_connections.erase(hdl);
	m_connection_to_username.erase(hdl);
	m_users[username].channel.erase(hdl);

	if (m_users[username].channel.empty()) {
		log("User " + username + " has no active connections, cleaning up...");
		if (!leave_message[username].empty()) {
			log("User " + username + " has " +
			    std::to_string(leave_message[username].size()) + " pending messages");
		}
	}

	log("Connection closed for user: " + username);
}

void ChatServer::on_fail(connection_hdl hdl) {
	try {
		server::connection_ptr con = m_server.get_con_from_hdl(hdl);
		websocketpp::lib::error_code ec = con->get_ec();

		logger.log(Logger::Level::ERROR,
			std::string("Connection failed: ") + ec.category().name() +
			" [code=" + std::to_string(ec.value()) +
			"] " + ec.message() +
			" | remote=" + con->get_remote_endpoint());

		switch(ec.value()) {
			case websocketpp::transport::error::eof:
				logger.log(Logger::Level::WARNING, "Client disconnected");
				con->close(websocketpp::close::status::abnormal_close, "");
				break;
			default:
				logger.log(Logger::Level::ERROR, "Unknown error, closing connection");
				con->close(websocketpp::close::status::internal_endpoint_error, "");
		}

		std::lock_guard<std::mutex> guard(m_mutex);
		m_connections.erase(hdl);
	} catch (const websocketpp::exception& e) {
		logger.log(Logger::Level::ERROR, std::string("Failed to handle failed connection: ") + e.what());
	}
}

void ChatServer::send_ping() {
	for (auto &conn : m_connections) {
		m_server.ping(conn, "ping");
	}

	m_timer->expires_after(std::chrono::seconds(config.ping_interval_sec));
	m_timer->async_wait([this](const boost::system::error_code& ec) {
		if (ec) {
			log("heartbeat timer error: " + ec.message());
		}
		send_ping();
	});
}

void ChatServer::on_message(connection_hdl hdl, server::message_ptr msg) {
	std::unique_lock<std::mutex> lock(m_mutex);
	try {
		json j = json::parse(msg->get_payload());

		if (!j.contains("type") || !j["type"].is_string()) {
			log("Message does not contain a valid 'type' field.");
			send_response(hdl, "warn", "Invalid message format: missing or invalid 'type'.");
			return;
		}

		std::string type = j["type"];
		if (type == "login") {
			handle_login(j, hdl);
		} else if (type == "guest") {
			handle_guest(j, hdl);
		} else {
			if (!is_logged_in(hdl)) {
				send_response(hdl, "warn", "User not logged in");
				return;
			}
			if (type == "message") {
				handle_message(j, hdl);
			} else if (type == "uploadfile") {
				lock.unlock();
				std::thread t(&ChatServer::handle_file_upload, this, j, hdl);
				t.detach();
				return;
			} else {
				log("Unknown message type: " + type);
				send_response(hdl, "warn", "Unknown message type: " + type);
			}
		}
	} catch (const json::exception &e) {
		log(std::string("JSON Error: ") + e.what());
		send_response(hdl, "warn", "Invalid JSON format or structure: " + std::string(e.what()));
		m_server.close(hdl, websocketpp::close::status::going_away, "Invalid JSON");
	} catch (const std::exception &e) {
		log(std::string("Error processing message: ") + e.what());
		send_response(hdl, "warn", "Server error: " + std::string(e.what()));
		m_server.close(hdl, websocketpp::close::status::going_away, "Error");
	} catch (...) {
		log("Unknown error processing message.");
		send_response(hdl, "warn", "Unknown server error.");
		m_server.close(hdl, websocketpp::close::status::going_away, "Error");
	}
}

void ChatServer::log(const std::string &message) {
	logger.log(Logger::Level::INFO, message);
}

bool ChatServer::is_valid_username(const std::string &username) {
	if (username.length() < config.min_username_len || username.length() > config.max_username_len) {
		return false;
	}

	std::regex pattern("^[a-zA-Z0-9_]+$");
	return std::regex_match(username, pattern);
}

bool ChatServer::is_valid_password(const std::string &password) {
	std::regex pattern("^[a-zA-Z0-9_]+$");
	return std::regex_match(password, pattern) && !password.empty();
}

bool ChatServer::is_valid_message(const std::string &message) {
	return message.length() <= config.max_message_len;
}

bool ChatServer::is_logged_in(connection_hdl hdl) {
	return m_connection_to_username.find(hdl) != m_connection_to_username.end();
}

int ChatServer::level(connection_hdl hdl) {
	std::string username = get_username_by_connection(hdl);
	return m_users[username].level;
}

std::string ChatServer::extractIPv4(const std::string& address) {
	std::regex ipv4_pattern(R"((\d+\.\d+\.\d+\.\d+)|(::ffff:\d+\.\d+\.\d+\.\d+))");
	std::smatch match;
	if (std::regex_search(address, match, ipv4_pattern)) {
		return match[0].str();
	}
	return address;
}

std::string ChatServer::get_IP_from_hdl(connection_hdl hdl) {
	server::connection_ptr con = ChatServer::m_server.get_con_from_hdl(hdl);
	std::string remote_endpoint = con->get_remote_endpoint();
	std::string result = "";
	for (size_t i = 0, sz = remote_endpoint.size(); i < sz; i++) {
		if (remote_endpoint[i] != ':') {
			result += remote_endpoint[i];
		} else {
			break;
		}
	}
	std::string client_ip = con->get_request_header("X-Forwarded-For");
	if (!client_ip.empty()) {
		return client_ip;
	} else {
		return result;
	}
}

std::string ChatServer::GenerateToken(std::string username) {
	for (auto &conn : m_connections) {
		if (get_username_by_connection(conn) == username) {
			std::string tm = getCurrentTime(), First = username + m_users[username].password + get_IP_from_hdl(conn) + tm;
			std::string Second = sha256(First);
			std::string Third = sha256(Second);
			tokens[Third].username = username;
			tokens[Third].IP = get_IP_from_hdl(conn);
			tokens[Third].time = tm;
			return Third;
		}
	}
	return "false";
}

bool ChatServer::RemoveToken(std::string Token) {
	if(tokens[Token].username != "") {
		tokens[Token].username = "";
		return true;
	} else {
		return false;
	}
}

bool ChatServer::check_rate_limit(connection_hdl hdl, bool type) {
	std::string username = get_username_by_connection(hdl);
	auto now = std::chrono::steady_clock::now();
	auto time_since_last_login = std::chrono::duration_cast<std::chrono::seconds>(now - m_users[username].last_login_time).count();
	auto time_since_last_message = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_users[username].last_message_time).count();
	auto message_timer = std::chrono::duration_cast<std::chrono::seconds>(now - m_users[username].MessageTimer).count();

	if (type) {
		if (time_since_last_message < config.rate_limit_msg_interval_ms) {
			return true;
		}

		if (m_users[username].message_count > config.rate_limit_msg_burst) {
			if (message_timer > config.rate_limit_msg_window_sec) {
				m_users[username].message_count = 0;
				m_users[username].MessageTimer = now;

			} else {
				return true;
			}
		}

		m_users[username].last_message_time = now;
		} else {
		if (time_since_last_login < config.rate_limit_login_interval_sec) {
			return true;
		}
		m_users[username].last_login_time = now;
	}
	return false;
}

bool ChatServer::validate_command_args(const std::vector<std::string>& parts, size_t min_args,
                                       connection_hdl hdl, const std::string& usage) {
	if (parts.size() < min_args) {
		send_response(hdl, "warn", "Usage: " + usage);
		return false;
	}
	return true;
}

void ChatServer::send_online_user_list(connection_hdl hdl, const std::string& username) {
	std::map<std::string, bool> book;
	std::string result = "User: " + username + ", ";
	log("Send online user list to " + username);
	for (auto &it : channel[m_users[username].channel[hdl]].list) {
		if (book[it] == false && it != username) {
			result += it + ", ";
			book[it] = true;
		}
	}
	result = result.substr(0, result.size() - 2);
	send_response(hdl, "info", result);
}

void ChatServer::notify_channel_join(const std::string& username, const std::string& channel_name) {
	if (channel[channel_name].list.count(username) == 0) {
		for (auto &conn : m_connections) {
			if (m_users[get_username_by_connection(conn)].channel[conn] == channel_name) {
				send_response(conn, "info", username + " joined channel");
			}
		}
	}
	channel[channel_name].list.insert(username);
}

void ChatServer::notify_channel_leave(const std::string& username, const std::string& channel_name, connection_hdl hdl) {
	if (channel[channel_name].list.count(username) == 0 && username != "") {
		for (auto &conn : m_connections) {
			if (conn.lock() == hdl.lock()) continue;
			std::string target_user = get_username_by_connection(conn);
			if (!target_user.empty() && m_users[target_user].channel[conn] == channel_name) {
				send_response(conn, "info", username + " left channel");
			}
		}
	}
}

bool ChatServer::check_banned_ip(connection_hdl hdl) {
	for (auto &it : m_banned_users) {
		if (it.first == get_IP_from_hdl(hdl)) {
			log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
			send_response(hdl, "warn", "IP is banned");
			return true;
		}
	}
	return false;
}

bool ChatServer::check_banned_user(const std::string& username, connection_hdl hdl) {
	for (auto &it : m_banned_users) {
		if (it.second == username) {
			log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
			send_response(hdl, "warn", "User is banned");
			return true;
		}
	}
	return false;
}

bool ChatServer::check_lock_restriction(const std::string& channel_name, connection_hdl hdl,
                                         const std::string& username) {
	std::string user = username.empty() ? get_username_by_connection(hdl) : username;
	if ((channel[channel_name].IsLock || IsLockSite) &&
	    m_permission_groups[m_users[user].permission_group].permissions.count("joinlockroom") == 0) {
		send_response(hdl, "warn", "Cannot join locked channel/site");
		log(user + " attempted to join a lock room");
		return true;
	}
	return false;
}

bool ChatServer::has_permission(connection_hdl hdl, const std::string& perm) {
	std::string username = get_username_by_connection(hdl);
	if (m_permission_groups[m_users[username].permission_group].permissions.count(perm) == 0) {
		send_response(hdl, "warn", "Insufficient permissions");
		log("User " + username + " attempted to use " + perm + " but did not have sufficient permissions");
		return false;
	}
	return true;
}

void ChatServer::track_login_ip(const std::string& username, const std::string& ip) {
		if (!m_db) return;
		sqlite3_stmt* stmt = nullptr;
		const char* sql = "INSERT INTO login_history(username, ip, login_time) VALUES(?,?,?)";
		if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
			std::string now = getCurrentTime();
			sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) != SQLITE_DONE) {
				log(std::string("Failed to track login IP: ") + sqlite3_errmsg(m_db));
			}
			sqlite3_finalize(stmt);
		}
	}

std::string ChatServer::get_login_ips(const std::string& username, int limit) {
		if (!m_db) return "";
		sqlite3_stmt* stmt = nullptr;
		std::string sql = "SELECT ip, login_time FROM login_history WHERE username=? ORDER BY login_time DESC LIMIT ?";
		std::string result;
		if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, limit);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				if (!result.empty()) result += ", ";
				result += std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
				result += " (" + std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + ")";
			}
			sqlite3_finalize(stmt);
		}
		return result;
	}

void ChatServer::save_json_file(const std::string& filepath, const json& j,
                                 const std::string& empty_content) {
	std::ofstream file(filepath);
	if (file.is_open()) {
		try {
			std::string content = j.dump(4);
			if (content.back() != '\n') {
				content += '\n';
			}
			file << content;
		} catch (const std::exception &e) {
			log(std::string("Error saving ") + filepath + ": " + e.what());
		} catch (...) {
			log("Unknown error saving " + filepath);
		}
	} else {
		log("Could not open " + filepath + " for writing.");
	}
}

std::string ChatServer::hash_password(const std::string& password, const std::string& salt) {
	if (salt.empty()) {
		return sha256(password);
	}
	return sha256(salt + password);
}

std::string ChatServer::generate_salt() {
	static std::mt19937 rng(std::random_device{}());
	static const std::string chars =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()";
	std::string salt;
	for (int i = 0; i < 16; ++i) {
		salt += chars[rng() % chars.size()];
	}
	return salt;
}

void ChatServer::handle_login(const json &j, connection_hdl hdl) {
	load_banned_users();
	for (auto &it : m_banned_users) {
		if (it.first == get_IP_from_hdl(hdl)) {
			log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
			send_response(hdl, "warn", "IP is banned");
			m_server.close(hdl, websocketpp::close::status::normal, "IP is banned");
			return;
		}
	}
	if (check_rate_limit(hdl, false)) {
		log("Username " + get_username_by_connection(hdl) + " Rate limit exceeded.");
		send_response(hdl, "warn", "Login too frequent");
		return;
	}
	if (j.contains("token")) {
		std::string token = j["token"];
		if (tokens[token].username != "") {
			std::string username = tokens[token].username;
			std::string user_channel = m_users[tokens[token].username].channel[hdl];
			for (auto &it : m_banned_users) {
				if (it.second == username) {
					log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
					send_response(hdl, "warn", "User is banned");
					m_server.close(hdl, websocketpp::close::status::normal, "User is banned");
					return;
				}
			}
			if ((channel[user_channel].IsLock || IsLockSite) && m_permission_groups[m_users[username].permission_group].permissions.count("joinlockroom") == 0) {
				send_response(hdl, "warn", "Cannot join locked channel/site");
				log(username + " attempted to join a lock room");
				return;
			}
			m_connection_to_username[hdl] = username;
			m_users[username].channel[hdl] =  (user_channel == "" ? "main" : user_channel);
			user_channel = (user_channel == "" ? "main" : user_channel);
			track_login_ip(username, get_IP_from_hdl(hdl));
			log("User logged in: " + username);
			send_response(hdl, "info", "Login successful.");
			m_users[username].MessageTimer = std::chrono::steady_clock::now();
			m_users[username].message_count = 0;

			send_online_user_list(hdl, username);
			notify_channel_join(username, user_channel);
			if (!leave_message[username].empty()) {
				send_response(hdl, "info", "You have " + std::to_string(leave_message[username].size()) + " messages, use /lw to view");
			}
			return;
		} else {
			log("User try to use token but error.");
			send_response(hdl, "warn", "Token expired/invalid");
			m_server.close(hdl, websocketpp::close::status::going_away, "Token expired/invalid");
			return;
		}
	}

	if (!j.contains("username") || !j["username"].is_string() ||
	        !j.contains("password") || !j["password"].is_string() ||
	        !j.contains("channel")  || !j["channel"].is_string()) {
		log("Login message missing or invalid required fields (username, password, channel).");
		send_response(hdl, "warn", "Invalid login format: missing or invalid fields.");
		return;
	}

	std::string username = j["username"];
	std::string user_channel = j["channel"];
	std::string password = sha256(config.password_salt + j["password"].get<std::string>());

	for (auto &it : m_banned_users) {
		if (it.second == username) {
			log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
			send_response(hdl, "warn", "User is banned");
			m_server.close(hdl, websocketpp::close::status::normal, "User is banned");
			return;
		}
	}
	if (!is_valid_username(username)) {
		log("Invalid username format.");
		send_response(hdl, "warn", "Invalid username format");
		return;
	}
	if (password.empty()) {
		log("Password cannot be empty.");
		send_response(hdl, "warn", "Password cannot be empty");
		return;
	}
	load_users();
	if (m_users.find(username) != m_users.end()) {
		if (m_users[username].password == password) {
			if ((channel[user_channel].IsLock || IsLockSite) && m_permission_groups[m_users[username].permission_group].permissions.count("joinlockroom") == 0) {
				send_response(hdl, "warn", "Cannot join locked room");
				log(username + " attempted to join a lock room");
				return;
			}
			m_connection_to_username[hdl] = username;
			if (user_channel == "") user_channel = "main";
			m_users[username].channel[hdl] = user_channel;
			std::string token = GenerateToken(username);
			json Return;
			Return["token"] = token;
			m_server.send(hdl, Return.dump(), websocketpp::frame::opcode::text);
			log("User logged in: " + username);
			send_response(hdl, "info", "Login successful.");
			m_users[username].MessageTimer = std::chrono::steady_clock::now();
			m_users[username].message_count = 0;

			send_online_user_list(hdl, username);
			notify_channel_join(username, user_channel);
			if (!leave_message[username].empty()) {
				send_response(hdl, "info", "You have " + std::to_string(leave_message[username].size()) + " messages, use /lw to view");
			}
			return;
		} else {
			log("Login failed for user: " + username);
			send_response(hdl, "warn", "Incorrect password");
		}
	} else {

		std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> temp;
		temp[hdl] = user_channel;
		m_users[username] = UserInfo {password, 0, true, "normal", temp};
		temp.clear();
		save_users();
		if (channel[user_channel].IsLock || IsLockSite) {
			send_response(hdl, "warn", "Cannot join locked room");
			log(username + " attempted to join a lock room");
			return;
		}
		if (m_IPs[get_IP_from_hdl(hdl)].size() == config.max_registrations_per_ip) {
			send_response(hdl, "warn", "Registration limit reached for this IP");
			log(username + " attempted to register");
			return;
		}
		m_IPs[get_IP_from_hdl(hdl)].insert(username);
		m_connection_to_username[hdl] = username;
		std::string token = GenerateToken(username);
		json Return;
		Return["token"] = token;
		m_server.send(hdl, Return.dump(), websocketpp::frame::opcode::text);
		track_login_ip(username, get_IP_from_hdl(hdl));
		log("User registered and logged in: " + username);
		send_response(hdl, "info", "Registered and logged in successfully");
		m_users[username].MessageTimer = std::chrono::steady_clock::now();
			m_users[username].message_count = 0;

		send_online_user_list(hdl, username);
		notify_channel_join(username, user_channel);
	}
}

void ChatServer::handle_guest(const json &j, connection_hdl hdl) {
	if (!j.contains("channel") || !j["channel"].is_string()) {
		log("Guest login message missing or invalid 'channel' field.");
		send_response(hdl, "warn", "Invalid guest login format: missing or invalid 'channel'.");
		return;
	}

	load_banned_users();
	for (auto &it : m_banned_users) {
		if (it.first == get_IP_from_hdl(hdl)) {
			log("Banned user attempted to login: " + get_IP_from_hdl(hdl));
			send_response(hdl, "warn", "IP is banned");
			m_server.close(hdl, websocketpp::close::status::normal, "IP is banned");
			return;
		}
	}

	std::string user_channel = j["channel"];
	std::string username = "Guest " + sha256(get_IP_from_hdl(hdl)).substr(0, config.guest_name_len) + GetRandomString(config.guest_name_len);

	if ((channel[user_channel].IsLock || IsLockSite) && m_users[username].level < 1) {
		send_response(hdl, "warn", "Cannot join locked room");
		log(username + " attempted to join a lock room");
		return;
	}

	m_connection_to_username[hdl] = username;
	if (user_channel == "") user_channel = "main";
	m_users[username].channel[hdl] = user_channel;
	m_users[username].level = 0;
	log("Guest logged in: " + username);
	send_response(hdl, "info", "Login successful.");
	m_users[username].MessageTimer = std::chrono::steady_clock::now();
			m_users[username].message_count = 0;

	send_online_user_list(hdl, username);
	notify_channel_join(username, user_channel);
	if (!leave_message[username].empty()) {
		send_response(hdl, "info", "You have " + std::to_string(leave_message[username].size()) + " messages, use /lw to view");
	}
	return;
}

void ChatServer::handle_file_upload(const json &j, connection_hdl hdl) {
	try {
		std::string username = get_username_by_connection(hdl);
		if (username.empty()) {
			send_response(hdl, "warn", "User not logged in");
			return;
		}

		if (!j.contains("filename") || !j["filename"].is_string() ||
		        !j.contains("data") || !j["data"].is_string()) {
			send_response(hdl, "warn", "Invalid file upload format: missing or invalid fields.");
			return;
		}

		if (m_permission_groups[m_users[username].permission_group].permissions.count("file") == 0) {
			send_response(hdl, "warn", "Insufficient permissions");
			return;
		}

		std::string filename = j["filename"];
		std::string file_content = j["data"];

		if (filename.find("..") != std::string::npos ||
		        filename.find('/') != std::string::npos ||
		        filename.find('\\') != std::string::npos) {
			send_response(hdl, "warn", "Invalid characters in filename");
			return;
		}

		fs::path file_path(filename);
		std::string ext = file_path.extension().string();

		static std::mt19937 file_rng(std::random_device{}());
		std::string FileID = sha256(std::to_string(file_rng() % 100000) + "_" + filename + "_" + username + "_" + sha256(file_content));
		m_files[FileID + ext] = {username, FileID + ext, filename, false};
		m_users[username].UserFile.insert(FileID + ext);

		filename = FileID + ext;
		fs::path full_path = "files/" + filename;

		if (!fs::exists("files")) {
			fs::create_directory("files");
		}

		std::string decoded_content = base64_decode(file_content);
		std::ofstream out_file(full_path, std::ios::binary);
		if (!out_file.is_open()) {
			log("Failed to open file for writing: " + full_path.string());
			send_response(hdl, "warn", "Failed to write file");
			return;
		}

		out_file.write(decoded_content.data(), static_cast<std::streamsize>(decoded_content.size()));
		if (!out_file.good()) {
			log("File write error: " + full_path.string());
			send_response(hdl, "warn", "Failed to write file");
			out_file.close();
			fs::remove(full_path);
			return;
		}
		out_file.close();

		log("User " + username + " uploaded file: " + filename);
		send_response(hdl, "info", "File uploaded: " + FileID + ext);
		save_users();
		save_files();

	} catch (const std::exception &e) {
		log(std::string("File upload error: ") + e.what());
		send_response(hdl, "warn", "File upload failed: " + std::string(e.what()));
	}
}

void ChatServer::handle_message(const json &j, connection_hdl hdl) {
	if (!j.contains("message") || !j["message"].is_string()) {
		log("Message missing or invalid 'message' field.");
		send_response(hdl, "warn", "Invalid message format: missing or invalid 'message'.");
		return;
	}

	if (check_rate_limit(hdl, true)) {
		log("Username " + get_username_by_connection(hdl) + " Rate limit exceeded.");
		send_response(hdl, "warn", "Message rate limit exceeded");
		return;
	}

	std::string username = get_username_by_connection(hdl);
	std::string content = j["message"];

	m_users[username].message_count++;
	if (!is_valid_message(content)) {
		m_users[username].message_count += int(content.length() / 100) - 1;
	}

	if (username == "") {
		send_response(hdl, "warn", "User not logged in");
		return;
	}

	if (startsWith(content, "/")) {
		log("Command from " + username + ": " + content);

		if (startsWith(content, "/help")) {
			log("Send help message to " + username);
			if (content == "/help") {
				std::string HelpMessage = "### Commands\n\n - `/help [command]` - Show help (perm: 0)\n - `/user_list` - Show online users (perm: 0)\n - `/w <user> <message>` - Send whisper (perm: 0)\n - `/lw [user] [message]` - View/send leave msg (perm: 0)\n - `/join <channel>` - Join or switch channel (perm: 0)\n - `/me <action>` - Send action message (perm: 0)\n - `/ignore add/remove <user>` - Block/unblock (perm: 0)\n - `/permission` - View permission group (perm: 0)\n - `/level` - View permission level (perm: 0)\n - `/token_list` - List tokens (perm: 0)\n - `/rmtoken <token>` - Remove token (perm: 0)\n - `/setpwd [user] <password>` - Set password (perm: 0/2)\n - `/file [action]` - File management (perm: varies)\n - `/kick <user>` - Kick user (perm: 1)\n - `/ban_list` - Ban list (perm: 1)\n - `/ban <user>` - Ban user (perm: 2)\n - `/unban <IP/user>` - Unban (perm: 2)\n - `/unbanall` - Unban all (perm: 2)\n - `/mute <user>` - Mute user (perm: 2)\n - `/unmute <user>` - Unmute (perm: 2)\n - `/banip <IP>` - Ban IP directly (perm: 2)\n - `/gperm <action>` - Group permission mgmt (perm: 2)\n - `/setpwd <user> <password>` - Change pwd for others (perm: 2)\n - `/status user/channel <name>` - Query status (perm: 0/2)\n - `/lock site/channel <name>` - Lock/unlock (perm: 2)\n - `/channel_list` - Channel list (perm: 2)\n - `/boardcast <message>` - Broadcast (perm: 2)\n - `/log [filename]` - View logs (perm: 2)\n\nUse `/help <command>` for details, e.g. `/help w`";
				send_response(hdl, "info", HelpMessage);
			} else if (startsWith(content, "/help ")) {
				std::string command = splitString(content)[1];
				if (command == "w") {
					send_response(hdl, "info", "### `/w`\n\n - Permission: 0\n - Usage: `/w <username> <message>`\n - Send a private message\n - Use `/lw` if offline");
				} else if (command == "user_list") {
					send_response(hdl, "info", "### `/user_list`\n\n - Permission: 0\n - Usage: `/user_list`\n - List online users in current channel");
				} else if (command == "setpwd") {
					send_response(hdl, "info", "### `/setpwd`\n\n - Usage: `/setpwd <username> <password>`\n - Self: no permission needed\n - Others: requires level 2\n - Allowed: letters, digits, underscores");
				} else if (command == "help") {
					send_response(hdl, "info", "### `/help`\n\n - Permission: 0\n - Usage: `/help` - Show all commands\n - Usage: `/help <command>` - Show details\n - Example: `/help w`");
				} else if (command == "gperm") {
					send_response(hdl, "info", "### `/gperm`\n\n - Permission: 2\n - Usage: `/gperm list` - List groups\n - Usage: `/gperm set <user> <group>` - Set user group\n - Usage: `/gperm create <name> <level> [tag] [color]` - Create\n - Usage: `/gperm modify <group> <field> <value>` - Modify\n - Usage: `/gperm delete <group>` - Delete\n - Fields: tag, color, level, addperm, rmperm");
				} else if (command == "status") {
					send_response(hdl, "info", "### `/status`\n\n - Usage: `/status user <username>` (perm 0 for self)\n - Usage: `/status channel <name>` (perm 2)\n - Query user status, level, channel\n - List channel online users");
				} else if (command == "level") {
					send_response(hdl, "info", "### `/level`\n\n - Permission: 0\n - Usage: `/level`\n - View your permission level and group");
				} else if (command == "join") {
					send_response(hdl, "info", "### `/join`\n\n - Permission: 0\n - Usage: `/join <channel>`\n - Join or switch to a channel\n - Locked channels need joinlockroom perm");
				} else if (command == "kick") {
					send_response(hdl, "info", "### `/kick`\n\n - Permission: 1\n - Usage: `/kick <username>`\n - Kick a user from the server\n - Cannot kick users at or above your level");
				} else if (command == "ban") {
					send_response(hdl, "info", "### `/ban`\n\n - Permission: 2\n - Usage: `/ban <username>`\n - Ban a user and their IP\n - Banned users cannot login");
				} else if (command == "unban") {
					send_response(hdl, "info", "### `/unban`\n\n - Permission: 2\n - Usage: `/unban <IP or username>`\n - Unban an IP or user\n - Need the banned IP or username");
				} else if (command == "unbanall") {
					send_response(hdl, "info", "### `/unbanall`\n\n - Permission: 2\n - Usage: `/unbanall`\n - Unban all IPs and users\n - Use with caution!");
				} else if (command == "banip") {
					send_response(hdl, "info", "### `/banip`\n\n - Permission: 2\n - Usage: `/banip <IP>`\n - Ban an IP address directly\n - All users on that IP are disconnected");
				} else if (command == "ban_list") {
					send_response(hdl, "info", "### `/ban_list`\n\n - Permission: 1\n - Usage: `/ban_list`\n - List all banned users and IPs");
				} else if (command == "channel_list") {
					send_response(hdl, "info", "### `/channel_list`\n\n - Permission: 2\n - Usage: `/channel_list`\n - List all active channels");
				} else if (command == "mute") {
					send_response(hdl, "info", "### `/mute`\n\n - Permission: 2\n - Usage: `/mute <username>`\n - Mute a user (no channel messages)\n - Can still receive messages and whispers");
				} else if (command == "unmute") {
					send_response(hdl, "info", "### `/unmute`\n\n - Permission: 2\n - Usage: `/unmute <username>`\n - Unmute a user");
				} else if (command == "lock") {
					send_response(hdl, "info", "### `/lock`\n\n - Permission: 2\n - Usage: `/lock site` - Lock/unlock site\n - Usage: `/lock channel <name>` - Lock/unlock channel\n - Regular users cannot join when locked\n - Run again to unlock");
				} else if (command == "log") {
					send_response(hdl, "info", "### `/log`\n\n - Permission: 2\n - Usage: `/log` - View today's log\n - Usage: `/log <filename>` - View a log file\n - Log files are in logs/ directory");
				} else if (command == "boardcast") {
					send_response(hdl, "info", "### `/boardcast`\n\n - Permission: 2\n - Usage: `/boardcast <message>`\n - Broadcast to all online users");
				} else if (command == "lw") {
					send_response(hdl, "info", "### `/lw`\n\n - Permission: 0\n - Usage: `/lw` - View all messages\n - Usage: `/lw <username> <message>` - Leave a message\n - Recipient is notified on login");
				} else if (command == "file") {
					send_response(hdl, "info", "### `/file`\n\n - Permission: varies\n - Usage: `/file` - List your files\n - Usage: `/file download <ID>` - Download\n - Usage: `/file setprivate <ID> <true|false>` - Set private/public\n - Usage: `/file rename <ID> <new name>` - Rename\n - Usage: `/file remove <ID>` - Delete\n - Usage: `/file manage list [user]` - Manage all (managefile perm)\n - Usage: `/file manage download <ID>` - Admin download\n - Usage: `/file manage remove <ID>` - Admin delete");
				} else if (command == "token_list") {
					send_response(hdl, "info", "### `/token_list`\n\n - Permission: 0\n - Usage: `/token_list`\n - List your auth tokens\n - Tokens allow passwordless login");
				} else if (command == "rmtoken") {
					send_response(hdl, "info", "### `/rmtoken`\n\n - Permission: 0\n - Usage: `/rmtoken <token>`\n - Remove a token\n - Old tokens invalidate on IP change");
				} else if (command == "permission") {
					send_response(hdl, "info", "### `/permission`\n\n - Permission: 0\n - Usage: `/permission`\n - View your permission group name");
				} else if (command == "me") {
					send_response(hdl, "info", "### `/me`\n\n - Permission: 0\n - Usage: `/me <action>`\n - Send an action/emote message\n - Example: `/me waves` shows as `@username waves`");
				} else if (command == "ignore") {
					send_response(hdl, "info", "### `/ignore`\n\n - Permission: 0\n - Usage: `/ignore add <username>` - Block a user\n - Usage: `/ignore remove <username>` - Unblock\n - Blocked users' messages are hidden");
				} else {
					send_response(hdl, "warn", "Unknown command");
				}
			}
		} else if (startsWith(content, "/kick ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("kick") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the kick command but did not have sufficient permissions");
				return;
			} else {
				std::string target = splitString(content)[1];
				if (m_users.find(target) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					log("Admin " + username + " attempted to kicked user but can't find target");
					return;
				}
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == target) {
						if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[m_users[target].permission_group].level) {
							send_response(hdl, "warn", "Insufficient permission");
							log("Admin " + username + " attempted to kick user " + get_username_by_connection(conn) + " but did not have sufficient permissions");
							return;
						}
						send_response(conn, "warn", "You have been kicked by admin");
						m_server.close(conn, websocketpp::close::status::normal, "You kicked by admin.");
					}
				}
				log("Admin " + username + " kicked user: " + target);
				send_response(hdl, "info", "Kick successful");
			}
		} else if (startsWith(content, "/ban ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("ban") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the ban command but did not have sufficient permissions");
				return;
			} else {
				std::string target = splitString(content, 2)[1];
				if (m_users.find(target) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					log("Admin " + username + " attempted to banned user but can't find target");
					return;
				}
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == target) {
						if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[m_users[target].permission_group].level) {
							send_response(hdl, "warn", "Insufficient permission");
							log("User " + username + " attempted to use the ban command but did not have sufficient permissions");
							return;
						}
						m_banned_users[get_IP_from_hdl(conn)] = get_username_by_connection(conn);
						for (auto &conn1 : m_connections) {
							if (get_IP_from_hdl(conn1) == get_IP_from_hdl(conn) && get_username_by_connection(conn) != get_username_by_connection(conn1)) {
								send_response(conn1, "warn", "You have been banned by admin");
								m_server.close(conn1, websocketpp::close::status::normal, "You banned by admin.");
							}
						}
						log("Admin " + username + " banned IP: " + get_IP_from_hdl(conn));
						send_response(conn, "warn", "You have been banned by admin");
						m_server.close(conn, websocketpp::close::status::normal, "You banned by admin.");
					}
				}
				send_response(hdl, "info", "Ban successful");
				save_banned_users();
			}
		} else if (startsWith(content, "/unban ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("ban") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the unban command but did not have sufficient permissions");
				return;
			} else {
				bool flag = false;
				std::string target = splitString(content)[1];
				for (auto &it : m_banned_users) {
					if (it.first == target || it.second == target) {
						m_banned_users.erase(it.first);
						flag = true;
						break;
					}
				}
				if (flag) {
					save_banned_users();
					send_response(hdl, "info", "Unban successful");
					log("Admin " + username + "unban " + target);
				} else {
					send_response(hdl, "warn", "Target not found");
				}
			}
		} else if (startsWith(content, "/banip ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("ban") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the banip command but did not have sufficient permissions");
				return;
			}
			std::string target_ip = splitString(content, 2)[1];
			if (target_ip.empty()) {
				send_response(hdl, "warn", "Usage: /banip <IP>");
				return;
			}
			m_banned_users[target_ip] = "unknown";
			log("Admin " + username + " banned IP: " + target_ip);
			for (auto &conn : m_connections) {
				if (get_IP_from_hdl(conn) == target_ip) {
					send_response(conn, "warn", "You have been banned by admin");
					m_server.close(conn, websocketpp::close::status::normal, "You banned by admin.");
				}
			}
			save_banned_users();
			send_response(hdl, "info", "IP banned successfully");
		} else if (startsWith(content, "/mute ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("mute") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the mute command but did not have sufficient permissions");
				return;
			} else {
				std::string target = splitString(content, 2)[1];
				if (m_users.find(target) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					log("Admin " + username + " attempted to mute user but can't find target");
					return;
				}
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == target) {
						m_users[target].chat = false;
						send_response(conn, "info", "Chat permission has been changed");
						log("Admin " + username + " muted user: " + target);
					}
				}
				send_response(hdl, "info", "Mute successful");
				save_users();
			}
		} else if (startsWith(content, "/unmute ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("mute") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the unmnute command but did not have sufficient permissions");
				return;
			} else {
				std::string target = splitString(content, 2)[1];
				if (m_users.find(target) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					log("Admin " + username + " attempted to unmute user but can't find target");
					return;
				}
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == target) {
						m_users[target].chat = true;
						send_response(conn, "info", "Chat permission has been changed");
						log("Admin " + username + " unmuted user: " + target);
					}
				}
				send_response(hdl, "info", "Unmute successful");
				save_users();
			}
		} else if (content == "/unbanall") {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("ban") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the unbanall command but did not have sufficient permissions");
				return;
			} else {
				m_banned_users.clear();
				std::ofstream file("banned_users.json");
				if (file.is_open()) {
					try {
						std::string content = "{\"banned_users\":{}}";
						if (content.back() != '\n') {
							content += '\n';
						}
						file << content;
					} catch (const std::exception &e) {
						log(std::string("Error saving banned users: ") + e.what());
					} catch (...) {
						log("Unknown error saving banned users.");
					}
				} else {
					log("Could not open banned_users.json for writing.");
				}
				log("Admin " + username + " unbanned all IP.");
				send_response(hdl, "info", "Unban successful");
			}
		} else if (content == "/ban_list") {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("ban") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to get the banned user list but did not have sufficient permissions");
				return;
			} else {
				log("Send banned user list to " + username);
				std::string result = "Banned users:";
				std::map<std::string, std::queue<std::string>> book;
				for (const auto &[IP, username_] : m_banned_users) {
					if (IP != "" && username_ != "") book[username_].push(IP);
				}
				for (auto &[username_, IP_queue] : book) {
					result += "\n" + username_ + ": ";
					while (!IP_queue.empty()) {
						result += IP_queue.front() + ", ";
						IP_queue.pop();
					}
					result = result.substr(0, result.size() - 2);
				}
				send_response(hdl, "info", result);
			}
		} else if(content == "/user_list") {
			std::map<std::string, bool> book;
			std::string result = "Online users: ";
			log("Send online user list to " + username);
			for (auto &it : channel[m_users[username].channel[hdl]].list) {
				if (book[it] == false) {
					result += it + ", ";
					book[it] = true;
				}
			}
			result = result.substr(0, result.size() - 2);
			send_response(hdl, "info", result);
			book.clear();
		} else if(content == "/channel_list") {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("channel_list") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to get the channel list but did not have sufficient permissions");
				return;
			} else {
				log("Send channel list to " + username);
				std::string result = "Channels:";
				std::map<std::string, ChannelInfo>::iterator iter;
				for (iter = channel.begin(); iter != channel.end(); ++iter) {
					if (iter->second.list.size() != 0) {
						result += iter->first + ", ";
					}
				}
				result = result.substr(0, result.size() - 2);
				send_response(hdl, "info", result);
			}
		} else if(startsWith(content, "/file")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("file") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use file but did not have sufficient permissions");
				return;
			}
			if (content == "/file") {
				log("Send file list to " + username);
				std::string result = "filelist\n";
				for (auto &it : m_users[username].UserFile) {
					if (m_files.find(it) == m_files.end() ||
					        m_files[it].master == "" ||
					        m_files[it].NickName == "" ||
					        m_files[it].FileName == "") continue;
					result += m_files[it].NickName + ":\n - ID: " + m_files[it].FileName + "\n - " +
					          (m_files[it].IsPrivate ? "Private" : "Public") + "\n\n";
				}
				send_response(hdl, "info", result);
			} else if (startsWith(content, "/file ")) {
				std::vector<std::string> parts = splitString(content);
				if (parts.size() < 2) {
					send_response(hdl, "warn", "Invalid command format.");
					return;
				}

				std::string type = parts[1];

				if (type == "download") {
					if (parts.size() < 3) {
						send_response(hdl, "warn", "Usage: /file download <filename>");
						return;
					}
					std::string filename = parts[2];
					if (m_users[username].UserFile.find(filename) == m_users[username].UserFile.end()) {
						send_response(hdl, "warn", "File not found");
						return;
					}
					std::string data;
					std::ifstream getfile("files/" + filename, std::ios::binary);
					if (getfile.is_open()) {
						std::stringstream buffer;
						buffer << getfile.rdbuf();
						data = buffer.str();
						getfile.close();
					} else {
						send_response(hdl, "warn", "Failed to read file");
						return;
					}
					json response;
					response["data"] = base64_encode(data);
					response["filename"] = m_files[filename].NickName;
					m_server.send(hdl, response.dump(), websocketpp::frame::opcode::text);
					send_response(hdl, "info", "File downloaded successfully");
				} else if (type == "setprivate") {
					if (parts.size() < 4) {
						send_response(hdl, "warn", "Usage: /file setprivate <filename> <true|false>");
						return;
					}
					std::string filename = parts[2], flag = parts[3];
					if (m_users[username].UserFile.find(filename) == m_users[username].UserFile.end()) {
						send_response(hdl, "warn", "File not found");
						return;
					}
					if (flag != "true" && flag != "false") {
						send_response(hdl, "warn", "Invalid format");
						return;
					}
					m_files[filename].IsPrivate = flag == "true";
					save_files();
					send_response(hdl, "info", "File permission updated");
				} else if (type == "rename") {
					if (parts.size() < 4) {
						send_response(hdl, "warn", "Usage: /file rename <filename> <new name>");
						return;
					}
					std::string filename = parts[2], newname = parts[3];
					if (m_users[username].UserFile.find(filename) == m_users[username].UserFile.end()) {
						send_response(hdl, "warn", "File not found");
						return;
					}
					m_files[filename].NickName = newname;
					save_files();
					send_response(hdl, "info", "File renamed successfully");
				} else if (type == "remove") {
					if (parts.size() < 3) {
						send_response(hdl, "warn", "Usage: /file remove <filename>");
						return;
					}
					std::string filename = parts[2];
					if (m_users[username].UserFile.find(filename) == m_users[username].UserFile.end()) {
						send_response(hdl, "warn", "File not found");
						return;
					}
					fs::remove("files/" + filename);
					m_users[username].UserFile.erase(filename);
					m_files.erase(filename);
					save_files();
					send_response(hdl, "info", "File deleted successfully");
				} else if (type == "manage") {
					if (m_permission_groups[m_users[username].permission_group].permissions.count("managefile") == 0) {
						send_response(hdl, "warn", "Insufficient permission");
						log("User " + username + " attempted to manage file but did not have sufficient permissions");
						return;
					}

					if (parts.size() < 3) {
						std::string help_msg = "filemanageoperation:\n";
						help_msg += "/file manage list - List users with file permission\n";
						help_msg += "/file manage list <username> - View user's file list\n";
						help_msg += "/file manage download <filename> - Download file (admin)\n";
						help_msg += "/file manage remove <filename> - Delete file (admin)\n";
						send_response(hdl, "info", help_msg);
						return;
					}

					std::string operator_type = parts[2];

					if (operator_type == "list") {
						if (parts.size() == 3) {
							std::string result = "Users with file permission:\n";
							for (const auto& [name, info] : m_users) {
								if (m_permission_groups[info.permission_group].permissions.count("file") != 0) {
									result += name + ":  uploaded " + std::to_string(info.UserFile.size()) + " files\n";
								}
							}
							result += "Use /file manage list <username> to view user files";
							send_response(hdl, "info", result);
						} else if (parts.size() >= 4) {
							std::string target = parts[3];
							if (m_users.find(target) == m_users.end()) {
								send_response(hdl, "warn", "User does not exist");
								return;
							}
							if (m_permission_groups[m_users[target].permission_group].permissions.count("file") == 0) {
								send_response(hdl, "warn", "User has no file permission");
								return;
							}
							log("Send " + target + "'s file list to " + username);
							std::string result = target + "'s files:\n";
							for (auto &it : m_users[target].UserFile) {
								if (m_files.find(it) == m_files.end() ||
								        m_files[it].master == "" ||
								        m_files[it].NickName == "" ||
								        m_files[it].FileName == "") continue;
								result += m_files[it].NickName + ":\n - ID: " + m_files[it].FileName + "\n - " +
								          (m_files[it].IsPrivate ? "Private" : "Public") + "\n\n";
							}
							send_response(hdl, "info", result);
						}
					} else if (operator_type == "download") {
						if (parts.size() < 4) {
							send_response(hdl, "warn", "Usage: /file manage download <filename>");
							return;
						}
						std::string filename = parts[3];
						if (m_files.find(filename) == m_files.end()) {
							send_response(hdl, "warn", "File does not exist");
							return;
						}
						std::string data;
						std::ifstream getfile("files/" + filename, std::ios::binary);
						if (getfile.is_open()) {
							std::stringstream buffer;
							buffer << getfile.rdbuf();
							data = buffer.str();
							getfile.close();
						} else {
							send_response(hdl, "warn", "Failed to read file");
							return;
						}
						json response;
						response["data"] = base64_encode(data);
						response["filename"] = m_files[filename].NickName;
						m_server.send(hdl, response.dump(), websocketpp::frame::opcode::text);
						send_response(hdl, "info", "File downloaded successfully");
					} else if (operator_type == "remove") {
						if (parts.size() < 4) {
							send_response(hdl, "warn", "Usage: /file manage remove <filename>");
							return;
						}
						std::string filename = parts[3];
						if (m_files.find(filename) == m_files.end()) {
							send_response(hdl, "warn", "File does not exist");
							return;
						}
						if (m_users.find(m_files[filename].master) != m_users.end()) {
							m_users[m_files[filename].master].UserFile.erase(filename);
						}
						m_files.erase(filename);
						fs::remove("files/" + filename);
						save_files();
						save_users();
						send_response(hdl, "info", "File deleted successfully");
					} else {
						send_response(hdl, "warn", "Unknown management action");
					}
				} else {
					send_response(hdl, "warn", "Unknown file action");
				}
			}

		} else if (startsWith(content, "/w ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("w") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the w command but did not have sufficient permissions");
				return;
			}
			std::vector<std::string> parts = splitString(content, 3);
			if (parts.size() < 3) {
				send_response(hdl, "warn", "Invalid format");
				return;
			}
			std::string name = parts[1], msg = parts[2];
			if (m_users.find(name) == m_users.end()) {
				send_response(hdl, "warn", "User not found");
				return;
			}
			log("Message from " + username + " to " + name + ": " + content);
			bool flag = false;
			json response;
			response["type"] = "message";
			response["username"] = "[Whisper from]" + username;
			response["content"] = msg;
			for (auto &conn : m_connections) {
				if (get_username_by_connection(conn) == name) {
					if (m_users[name].ignore_users.count(username) == 1) {
						send_response(hdl, "warn", "You have been blocked by this user");
						return;
					}
					m_server.send(conn, response.dump(), websocketpp::frame::opcode::text);
					json Return;
					Return["type"] = "message";
					Return["username"] = "[Whisper]" + name;
					Return["content"] = msg;
					m_server.send(hdl, Return.dump(), websocketpp::frame::opcode::text);
					flag = true;
				}
			}
			if (flag) return;
			send_response(hdl, "warn", "User offline, use /lw <username> <message> to leave a message");
		} else if (startsWith(content, "/lw")) {
			if (startsWith(content, "/lw ")) {
				if (m_permission_groups[m_users[username].permission_group].permissions.count("lw") == 0) {
					send_response(hdl, "warn", "Insufficient permission");
					log("User " + username + " attempted to use the lw command but did not have sufficient permissions");
					return;
				}
				std::vector<std::string> parts = splitString(content, 3);
				if (parts.size() < 3) {
					send_response(hdl, "warn", "Invalid format");
					return;
				}
				std::string name = parts[1], msg = parts[2];
				if (m_users.find(name) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					return;
				}
				log("User " + username + " leave message to " + name + ": " + msg);
				leave_message[name].push("[" + getCurrentTime() + "] " + username + ": " + msg);
				send_response(hdl, "info", "Message left");
			} else if (content == "/lw") {
				if (leave_message[username].empty()) {
					send_response(hdl, "warn", "No messages");
				} else {
					std::string result = "\nmessagelist\n";
					while (leave_message[username].size()) {
						result += leave_message[username].front() + "\n";
						leave_message[username].pop();
					}
					send_response(hdl, "info", result);
				}
			}
		} else if (startsWith(content, "/gperm ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("setpms") == 0 &&
				m_permission_groups[m_users[username].permission_group].level < 2) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use gperm but did not have sufficient permissions");
				return;
			}
			std::vector<std::string> parts = splitString(content, 5);
			if (parts.size() < 2) {
				send_response(hdl, "warn", "Usage: /gperm <create|set|modify|delete|list> ...");
				return;
			}
			std::string action = parts[1];

			if (action == "list") {
				std::string result = "Permission groups:\n";
				for (const auto& [gname, ginfo] : m_permission_groups) {
					result += " - " + gname + " (level: " + std::to_string(ginfo.level) + ")";
					if (!ginfo.tag.empty()) result += " tag: " + ginfo.tag;
					result += "\n";
				}
				send_response(hdl, "info", result);
			} else if (action == "set") {
				if (parts.size() < 4) {
					send_response(hdl, "warn", "Usage: /gperm set <username> <group>");
					return;
				}
				std::string name = parts[2];
				std::string group = parts[3];
				if (m_users.find(name) == m_users.end()) {
					send_response(hdl, "warn", "User not found");
					return;
				}
				if (m_permission_groups.find(group) == m_permission_groups.end()) {
					send_response(hdl, "warn", "Group not found");
					return;
				}
				if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[group].level && username != name) {
					send_response(hdl, "warn", "Insufficient permission");
					log("Admin " + username + " attempted to set permission but group level too high");
					return;
				}
				if (name == username) {
					send_response(hdl, "warn", "Cannot modify your own permission");
					return;
				}
				m_users[name].permission_group = group;
				save_users();
				log("User " + username + " set " + name + " permission to " + group);
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == name) {
						send_response(conn, "info", "Your permission has been changed");
						break;
					}
				}
				send_response(hdl, "info", "Permission set successfully");
			} else if (action == "create") {
				if (parts.size() < 4) {
					send_response(hdl, "warn", "Usage: /gperm create <name> <level> [tag] [color]");
					return;
				}
				std::string gname = parts[2];
				int glevel = std::stoi(parts[3]);
				if (m_permission_groups.find(gname) != m_permission_groups.end()) {
					send_response(hdl, "warn", "Group already exists");
					return;
				}
				if (m_permission_groups[m_users[username].permission_group].level <= glevel) {
					send_response(hdl, "warn", "Cannot create group at or above your level");
					return;
				}
				std::string tag = (parts.size() > 4) ? parts[4] : "";
				std::string color = (parts.size() > 5) ? parts[5] : "";
				PermissionGroup new_group;
				new_group.level = glevel;
				new_group.tag = tag;
				new_group.color = color;
				m_permission_groups[gname] = new_group;
				save_permission_groups();
				log("Admin " + username + " created permission group: " + gname);
				send_response(hdl, "info", "Group created");
			} else if (action == "modify") {
				if (parts.size() < 5) {
					send_response(hdl, "warn", "Usage: /gperm modify <group> <field> <value>");
					return;
				}
				std::string gname = parts[2];
				std::string field = parts[3];
				std::string value = parts[4];
				if (m_permission_groups.find(gname) == m_permission_groups.end()) {
					send_response(hdl, "warn", "Group not found");
					return;
				}
				if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[gname].level) {
					send_response(hdl, "warn", "Insufficient permission");
					return;
				}
				if (field == "tag") {
					m_permission_groups[gname].tag = value;
				} else if (field == "color") {
					m_permission_groups[gname].color = value;
				} else if (field == "level") {
					int new_level = std::stoi(value);
					if (m_permission_groups[m_users[username].permission_group].level <= new_level) {
						send_response(hdl, "warn", "Cannot set level at or above your own");
						return;
					}
					m_permission_groups[gname].level = new_level;
				} else if (field == "addperm") {
					m_permission_groups[gname].permissions.insert(value);
				} else if (field == "rmperm") {
					m_permission_groups[gname].permissions.erase(value);
				} else {
					send_response(hdl, "warn", "Unknown field: " + field);
					return;
				}
				save_permission_groups();
				log("Admin " + username + " modified group " + gname + " " + field + "=" + value);
				send_response(hdl, "info", "Group modified");
			} else if (action == "delete") {
				if (parts.size() < 3) {
					send_response(hdl, "warn", "Usage: /gperm delete <group>");
					return;
				}
				std::string gname = parts[2];
				if (m_permission_groups.find(gname) == m_permission_groups.end()) {
					send_response(hdl, "warn", "Group not found");
					return;
				}
				if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[gname].level) {
					send_response(hdl, "warn", "Insufficient permission");
					return;
				}
				m_permission_groups.erase(gname);
				save_permission_groups();
				log("Admin " + username + " deleted group: " + gname);
				send_response(hdl, "info", "Group deleted");
			} else {
				send_response(hdl, "warn", "Unknown action. Use: create, set, modify, delete, list");
			}
		} else if (startsWith(content, "/setpwd ")) {
			std::vector<std::string> parts = splitString(content, 3);
			if (parts.size() < 3) {
				send_response(hdl, "warn", "Invalid format");
				return;
			}
			std::string name = parts[1];
			std::string pwd = parts[2];
			if (m_users.find(name) == m_users.end()) {
				send_response(hdl, "warn", "User not found");
				return;
			}
			if ((m_permission_groups[m_users[username].permission_group].permissions.count("setpwd") == 0 && name != username) || (m_permission_groups[m_users[username].permission_group].permissions.count("setpwd") == 1 && m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[m_users[name].permission_group].level && name != username)) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to change " + name + "'s password but did not have sufficient permissions");
				return;
			}
			log(username + " set " + name + "'s password.");
			for (auto &conn : m_connections) {
				if (get_username_by_connection(conn) == name) {
					send_response(conn, "info", "Your password has been changed");
					m_users[name].password = sha256(config.password_salt + pwd);
					save_users();
					send_response(hdl, "info", "Set successfully");
					return;
				}
			}
		} else if (content == "/token_list") {
			log("Send token list to " + username);
			std::string result = "\ntokenlist\n";
			for (const auto &token : tokens) {
				if (token.second.username == username) result += "{" + token.first + ", " + token.second.IP + "}" + ", ";
			}
			result = result.substr(0, result.size() - 2);
			result += "\nIP: " + get_IP_from_hdl(hdl) + "\nUse /rmtoken to remove old tokens when IP changes";
			send_response(hdl, "info", result);
		} else if (startsWith(content, "/rmtoken ")) {
			log("User " + username + " try to remove token");
			std::string token = splitString(content)[1];
			if (tokens[token].username == username && RemoveToken(token)) {
				send_response(hdl, "info", "Removed token " + token);
			} else {
				send_response(hdl, "warn", "Token not found");
			}
		} else if (startsWith(content, "/status ")) {
			std::vector<std::string> parts = splitString(content, 3);
			if (parts.size() < 3) {
				send_response(hdl, "warn", "Invalid format");
				return;
			}
			std::string typ = parts[1];
			if (typ == "channel" && m_permission_groups[m_users[username].permission_group].level < 2) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to get channel status but did not have sufficient permissions");
				return;
			}
			std::string info = parts[2];
			if (typ == "user" && info != username && m_permission_groups[m_users[username].permission_group].level < 2) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to get " + info + "'s status but did not have sufficient permissions");
				return;
			}
			if (typ == "user") {
				std::string result = "";
				bool online = false;
				connection_hdl target_hdl;
				if (m_permission_groups[m_users[username].permission_group].level <= m_permission_groups[m_users[info].permission_group].level && info != username) {
					send_response(hdl, "warn", "Insufficient permission");
					log("Admin " + username + " attempted to get " + info + "'s status but did not have sufficient permissions");
					return;
				}
				for (auto &conn : m_connections) {
					if (get_username_by_connection(conn) == info) {
						online = true;
						target_hdl = conn;
						break;
					}
				}
				if (online) {
					result += "User: " + info + " (online)\n";
					result += "Level: " + std::to_string(m_permission_groups[m_users[info].permission_group].level) + "\n";
					result += "Group: " + m_users[info].permission_group + "\n";
					result += "Channel: ";
					for (auto &[hdl2, channel_name] : m_users[info].channel) {
						result += channel_name + ", ";
					}
					result = result.substr(0, result.size() - 2);
					result += "\n";
					result += "IP: " + get_IP_from_hdl(target_hdl);
					std::string ips = get_login_ips(info, 5);
					if (!ips.empty()) {
						result += "\nRecent IPs: " + ips;
					}
				} else {
					result += "User: " + info + " (offline)\n";
					result += "Level: " + std::to_string(m_permission_groups[m_users[info].permission_group].level) + "\n";
					result += "Group: " + m_users[info].permission_group + "\n";
					std::string ips = get_login_ips(info, 5);
					if (!ips.empty()) {
						result += "Recent IPs: " + ips + "\n";
					}
				}
				send_response(hdl, "info", result);
			} else if (typ == "channel") {
				std::string result = "";
				result += "Channel: " + info + "\n";
				result += "onlineUser: ";
				for (auto &conn : m_connections) {
					if (m_users[get_username_by_connection(conn)].channel[conn] == info) {
						result += get_username_by_connection(conn) + ", ";
					}
				}
				result = result.substr(0, result.size() - 2);
				send_response(hdl, "info", result);
			}
		} else if (startsWith(content, "/ignore ")) {
			std::vector<std::string> parts = splitString(content, 3);
			if (parts.size() < 3) {
				send_response(hdl, "warn", "Invalid format");
				return;
			}
			std::string typ = parts[1];
			std::string info = parts[2];
			if (typ == "add") {
				if (m_users.find(info) != m_users.end()) {
					if (m_users[username].ignore_users.count(info) == 1) {
						send_response(hdl, "warn", "User is already blocked");
						return;
					}
					m_users[username].ignore_users.insert(info);
					send_response(hdl, "info", "User blocked");
				} else {
					send_response(hdl, "warn", "User does not exist");
					return;
				}
			} else if (typ == "remove") {
				if (m_users.find(info) != m_users.end()) {
					if (m_users[username].ignore_users.count(info) == 0) {
						send_response(hdl, "warn", "User is not blocked");
						return;
					}
					m_users[username].ignore_users.erase(info);
					send_response(hdl, "info", "User unblocked");
				} else {
					send_response(hdl, "warn", "User does not exist");
					return;
				}
			}
			save_users();
		} else if (startsWith(content, "/boardcast ")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("boardcast") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to send boardcast message but did not have sufficient permissions");
				return;
			}
			std::string message = splitString(content, 2)[1];
			for (auto &conn : m_connections) {
				send_response(conn, "info", message);
			}
		} else if (startsWith(content, "/me ")) {
			std::string message = splitString(content, 2)[1];
			for (auto &conn : m_connections) {
				std::string target_user = get_username_by_connection(conn);
				if (!target_user.empty() && m_users[target_user].channel[conn] == m_users[username].channel[hdl]) {
					send_response(conn, "info", "@" + username + " " + message);
				}
			}
		} else if (content == "/permission") {
			send_response(hdl, "info", "Permission group: " + m_users[username].permission_group);
		} else if (content == "/level") {
			send_response(hdl, "info", "Permission level: " + std::to_string(m_users[username].level));
		} else if (startsWith(content, "/lock")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("lock") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to use the lock command but did not have sufficient permissions");
				return;
			}
			std::vector<std::string> parts = splitString(content, 3);
			if (parts.size() < 2) {
				send_response(hdl, "warn", "Invalid format");
				return;
			}
			std::string typ = parts[1];
			if (typ == "channel") {
				if (parts.size() < 3) {
					send_response(hdl, "warn", "Invalid format");
					return;
				}
				std::string channel_name = parts[2], result;
				channel[channel_name].IsLock = !channel[channel_name].IsLock;
				if (channel[channel_name].IsLock) {
					result = "Channel " + channel_name + " locked";
				} else {
					result = "Channel " + channel_name + " unlocked";
				}
				for (auto &conn : m_connections) {
					if (m_users[get_username_by_connection(conn)].channel[conn] == channel_name) {
						send_response(conn, "info", result);
					}
				}
			} else if (typ == "site") {
				std::string result;
				IsLockSite = !IsLockSite;
				if (IsLockSite) {
					result = "Site locked";
				} else {
					result = "Site unlocked";
				}
				for (auto &conn : m_connections) {
					send_response(conn, "info", result);
				}
			} else {
				send_response(hdl, "warn", "Invalid format");
			}
		} else if (startsWith(content, "/join ")) {
			std::string to_channel = splitString(content)[1];
			if ((channel[to_channel].IsLock || IsLockSite) && m_permission_groups[m_users[username].permission_group].permissions.count("joinlockroom") == 0) {
				send_response(hdl, "warn", "Cannot join: site or channel is locked");
				log(username + " attempted to join a lock room");
				return;
			}
			channel[m_users[username].channel[hdl]].list.erase(channel[m_users[username].channel[hdl]].list.find(username));
			notify_channel_leave(username, m_users[username].channel[hdl], hdl);
			log("User " + username + " left channel " + m_users[username].channel[hdl] + " and joined channel " + to_channel);
			m_users[username].channel[hdl] = to_channel;
			channel[to_channel].list.insert(username);
			send_response(hdl, "info", "Joined " + splitString(content)[1] + " channel");
			if (channel[to_channel].list.count(username) == 1) {
				for (auto &conn : m_connections) {
					if (m_users[get_username_by_connection(conn)].channel[conn] == m_users[username].channel[hdl] && get_username_by_connection(conn) != username) {
						send_response(conn, "info", username + " joined the channel");
					}
				}
			}
		} else if (startsWith(content, "/log")) {
			if (m_permission_groups[m_users[username].permission_group].permissions.count("test") == 0) {
				send_response(hdl, "warn", "Insufficient permission");
				log("User " + username + " attempted to get log file but did not have sufficient permissions");
				return;
			}
			if (startsWith(content, "/log ")) {
				std::string filename = splitString(content, 2)[1];
				std::string logs;
				std::ifstream getlog("logs/" + filename);
				if (getlog.is_open()) {
					std::stringstream buffer;
					buffer << getlog.rdbuf();
					logs = buffer.str();
					getlog.close();
				} else {
					send_response(hdl, "warn", "Failed to get log or file does not exist");
					return;
				}
				json response;
				response["logs"] = logs;
				m_server.send(hdl, response.dump(), websocketpp::frame::opcode::text);
				send_response(hdl, "info", "Log retrieved");
			} else if (content == "/log") {
				std::string logs;
				std::string tm = getCurrentTime();
				tm = tm.substr(0, tm.size() - 12);
				std::ifstream getlog("logs/[" + tm + "].log");
				if (getlog.is_open()) {
					std::stringstream buffer;
					buffer << getlog.rdbuf();
					logs = buffer.str();
					getlog.close();
				} else {
					send_response(hdl, "warn", "Failed to get log or file does not exist");
					return;
				}
				json response;
				response["logs"] = logs;
				m_server.send(hdl, response.dump(), websocketpp::frame::opcode::text);
				send_response(hdl, "info", "Log retrieved");
			}
		} else {
			send_response(hdl, "warn", "Unknown command");
		}
	} else {
		if (!m_users[username].chat) {
			send_response(hdl, "warn", "You are muted");
			return;
		}
		log("Message from " + username + ": " + content);
		json response;
		response["content"] = content;
		response["tag"] = m_permission_groups[m_users[username].permission_group].tag;
		response["color"] = m_permission_groups[m_users[username].permission_group].color;
		response["level"] = m_permission_groups[m_users[username].permission_group].level;
		response["username"] = username;
		response["type"] = "message";
		broadcast_message(response, username, m_users[username].channel[hdl], hdl);
	}
}

std::string ChatServer::get_username_by_connection(connection_hdl hdl) {
	auto it = m_connection_to_username.find(hdl);
	if (it != m_connection_to_username.end()) {
		return it->second;
	}
	return "";
}

void ChatServer::send_response(connection_hdl hdl, const std::string &type, const std::string &message) {
	json response;
	response["message"] = message;
	response["type"] = type;
	if (m_connections.find(hdl) == m_connections.end()) {
		return;
	}
	try {
		m_server.send(hdl, response.dump(), websocketpp::frame::opcode::text);
	} catch (const std::exception &e) {
		log(std::string("Error sending response: ") + e.what());
	} catch (...) {
		log("Unknown error sending response.");
	}
}

void ChatServer::broadcast_message(const json &message, const std::string &name, const std::string &channel, connection_hdl hdl) {
	for (auto &conn : m_connections) {
		if (m_users[get_username_by_connection(conn)].channel[conn] == m_users[name].channel[hdl]) {
			if (m_users[get_username_by_connection(conn)].ignore_users.count(name) == 1) {
				continue;
			}
			try {
				m_server.send(conn, message.dump(), websocketpp::frame::opcode::text);
			} catch (const std::exception &e) {
				log(std::string("Error broadcasting message: ") + e.what());
			} catch (...) {
				log("Unknown error broadcasting message.");
			}
		}
	}
}

void ChatServer::load_users() {
	if (!m_db) {
		log("Database not initialized, cannot load users.");
		return;
	}
	sqlite3_stmt* stmt = nullptr;
	const char* sql = "SELECT username, password, level, chat, permission_group, ignore_users, user_files, registered_ip FROM users";
	if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			std::string username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
			m_users[username].password = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
			m_users[username].level = sqlite3_column_int(stmt, 2);
			m_users[username].chat = sqlite3_column_int(stmt, 3) != 0;
			m_users[username].permission_group = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
			try {
				auto ign = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
				m_users[username].ignore_users = ign.get<std::set<std::string>>();
			} catch (...) {}
			try {
				auto uf = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
				m_users[username].UserFile = uf.get<std::set<std::string>>();
			} catch (...) {}
			if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
				std::string reg_ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
				m_IPs[reg_ip].insert(username);
			}
		}
		sqlite3_finalize(stmt);
	} else {
		log(std::string("Error loading users: ") + sqlite3_errmsg(m_db));
		sqlite3_finalize(stmt);
	}
}

void ChatServer::save_users() {
	if (!m_db) return;
	const char* sql = "INSERT OR REPLACE INTO users(username, password, level, chat, permission_group, ignore_users, user_files, registered_ip) VALUES(?,?,?,?,?,?,?,?)";
	for (const auto& [username, info] : m_users) {
		if (startsWith(username, "Guest ")) continue;
		sqlite3_stmt* stmt = nullptr;
		if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 2, info.password.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 3, (info.level < 0 || info.level > 4) ? 0 : info.level);
			sqlite3_bind_int(stmt, 4, info.chat ? 1 : 0);
			sqlite3_bind_text(stmt, 5, info.permission_group.c_str(), -1, SQLITE_TRANSIENT);
			json ign = info.ignore_users;
			std::string ign_str = ign.dump();
			sqlite3_bind_text(stmt, 6, ign_str.c_str(), -1, SQLITE_TRANSIENT);
			json uf = info.UserFile;
			std::string uf_str = uf.dump();
			sqlite3_bind_text(stmt, 7, uf_str.c_str(), -1, SQLITE_TRANSIENT);
			std::string reg_ip;
			for (const auto& [ip, users] : m_IPs) {
				if (users.count(username)) {
					reg_ip = ip;
					break;
				}
			}
			sqlite3_bind_text(stmt, 8, reg_ip.c_str(), -1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) != SQLITE_DONE) {
				log(std::string("Error saving user ") + username + ": " + sqlite3_errmsg(m_db));
			}
			sqlite3_finalize(stmt);
		}
	}
}

void ChatServer::load_permission_groups() {
	if (!m_db) {
		log("Database not initialized, cannot load permission groups.");
		return;
	}
	sqlite3_stmt* stmt = nullptr;
	const char* sql = "SELECT name, tag, color, level, permissions FROM permission_groups";
	if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
			m_permission_groups[name].tag = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
			m_permission_groups[name].color = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
			m_permission_groups[name].level = sqlite3_column_int(stmt, 3);
			try {
				auto perms = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
				m_permission_groups[name].permissions = perms.get<std::set<std::string>>();
			} catch (...) {}
		}
		sqlite3_finalize(stmt);
	} else {
		log(std::string("Error loading permission groups: ") + sqlite3_errmsg(m_db));
		sqlite3_finalize(stmt);
	}
}

void ChatServer::save_permission_groups() {
	if (!m_db) return;
	const char* sql = "INSERT OR REPLACE INTO permission_groups(name, tag, color, level, permissions) VALUES(?,?,?,?,?)";
	for (const auto& [name, info] : m_permission_groups) {
		sqlite3_stmt* stmt = nullptr;
		if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 2, info.tag.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 3, info.color.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 4, (info.level < 0 || info.level > 4) ? 0 : info.level);
			json perms = info.permissions;
			std::string perms_str = perms.dump();
			sqlite3_bind_text(stmt, 5, perms_str.c_str(), -1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) != SQLITE_DONE) {
				log(std::string("Error saving group ") + name + ": " + sqlite3_errmsg(m_db));
			}
			sqlite3_finalize(stmt);
		}
	}
}

void ChatServer::load_banned_users() {
	std::ifstream file("banned_users.json", std::ios::binary);
	if (file.is_open()) {
		try {
			json j;
			file >> j;
			if (j.contains("banned_users")) {
				for (auto& [IP, username] : j["banned_users"].items()) {
					m_banned_users[IP] = std::string(username["username"]);
				}
			}
		} catch (const std::exception &e) {
			log(std::string("Error loading banned users: ") + e.what());
		} catch (...) {
			log("Unknown error loading banned users.");
		}
	} else {
		log("Could not open banned_users.json for reading.");
	}
}

void ChatServer::save_banned_users() {
	std::thread([this]() {
		std::lock_guard<std::mutex> lock(m_mutex);
		json j;
		for (const auto& [IP, username] : m_banned_users) {
			if (IP != "" && username != "") {
				j["banned_users"][IP] = {{"username", username}};
			}
		}
		if (j.empty()) {
			save_json_file(config.banned_users_file, j, "{\"banned_users\":{}}");
		} else {
			save_json_file(config.banned_users_file, j);
		}
	}).detach();
}

void ChatServer::load_files() {
	std::ifstream file("files.json", std::ios::binary);
	if (file.is_open()) {
		try {
			json j;
			file >> j;
			if (j.contains("files")) {
				for (auto& [FileID, info] : j["files"].items()) {
					m_files[FileID] = {j["files"][FileID]["master"], j["files"][FileID]["name"], j["files"][FileID]["nick_name"], j["files"][FileID]["IsPrivate"]};
				}
			}
		} catch (const std::exception &e) {
			log(std::string("Error loading files: ") + e.what());
		} catch (...) {
			log("Unknown error loading files.");
		}
	} else {
		log("Could not open files.json for reading.");
	}
}

void ChatServer::save_files() {
	std::thread([this]() {
		std::lock_guard<std::mutex> lock(m_mutex);
		json j;
		for (const auto& [FileID, file_info] : m_files) {
			if (FileID == "" || file_info.master == "" || file_info.FileName == "" || file_info.NickName == "") {
				continue;
			}
			j["files"][FileID] = {{"master", m_files[FileID].master}, {"name", m_files[FileID].FileName}, {"nick_name", m_files[FileID].NickName}, {"IsPrivate", m_files[FileID].IsPrivate}};
		}
		if (j.empty()) {
			save_json_file(config.files_meta_file, j, "{\"files\":{}}");
		} else {
			save_json_file(config.files_meta_file, j);
		}
	}).detach();
}

std::string get_mime_type(const std::string& ext) {
	static const std::unordered_map<std::string, std::string> mime_types {
		{".html", "text/html"},
		{".htm",  "text/html"},
		{".txt",  "text/plain"},
		{".css",  "text/css"},
		{".js",   "application/javascript"},
		{".json", "application/json"},
		{".jpg",  "image/jpeg"},
		{".jpeg", "image/jpeg"},
		{".png",  "image/png"},
		{".gif",  "image/gif"},
		{".ico",  "image/x-icon"},
		{".svg",  "image/svg+xml"},
		{".pdf",  "application/pdf"},
		{".woff", "font/woff"},
		{".woff2", "font/woff2"},
		{".ttf", "font/ttf"},
		{".mp4", "video/mp4"},
		{".webm", "video/webm"}
	};
	auto it = mime_types.find(ext);
	return it != mime_types.end() ? it->second : "application/octet-stream";
}

std::string create_response(int status, const std::string& content, const std::string& mime, const std::string& filename = "") {
	std::stringstream ss;
	if (filename.empty()) {
		ss << "HTTP/1.1 " << status << " "
		   << (status == 200 ? "OK" : "Not Found") << "\r\n"
		   << "Content-Type: " << mime << "\r\n"
		   << "Content-Length: " << content.size() << "\r\n"
		   << "Connection: close\r\n\r\n"

		   << content;
	} else {
		ss << "HTTP/1.1 " << status << " "
		   << (status == 200 ? "OK" : "Not Found") << "\r\n"
		   << "Content-Type: " << mime << "\r\n"
		   << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n"
		   << "Content-Length: " << content.size() << "\r\n"
		   << "Connection: close\r\n\r\n"

		   << content;
	}
	return ss.str();
}

std::string parse_request_path(const std::string& request) {
	size_t start = request.find(' ');
	if (start == std::string::npos) return "";
	size_t end = request.find(' ', start + 1);
	if (end == std::string::npos) return "";
	std::string path = request.substr(start + 1, end - start - 1);
	return path == "/" ? "/index.html" : path;
}

void ChatServer::handle_request(Socket client, const fs::path& web_root) {
	try {
		std::string client_ip = client.get_peer_address();
		std::string request;
		try {
			request = client.Receive();
		} catch (const SocketException&) {
			logger.log(Logger::Level::WARNING, "Receive failed from " + client_ip);
			return;
		}
		logger.log(Logger::Level::INFO, "Request from " + client_ip + ": " + request.substr(0, request.find('\r')));

		std::string path = parse_request_path(request);
		fs::path file_path = web_root / path.substr(1);
		file_path = fs::weakly_canonical(file_path);

		fs::path canonical_web = fs::canonical(web_root);
		fs::path canonical_file = fs::canonical(file_path.parent_path());

		if (canonical_file.string().find(canonical_web.string()) != 0) {
			client.Send(create_response(403, "Forbidden", "text/plain"));
			return;
		}

		if (!fs::exists(file_path)) {
			throw std::runtime_error("File not found");
		}

		if (m_files.find(file_path.filename().string()) != m_files.end() && m_files[file_path.filename().string()].IsPrivate == true) {
			client.Send(create_response(404, "Not Found", "text/plain"));
			return;
		}

		std::ifstream file(file_path, std::ios::binary);
		if (!file) {
			throw std::runtime_error("Cannot open file");
		}

		std::string content((std::istreambuf_iterator<char>(file)),
		                    std::istreambuf_iterator<char>());
		std::string mime = get_mime_type(file_path.extension().string());

		std::string filename_to_send = "";
		if (m_files.find(file_path.filename().string()) != m_files.end()) {
			filename_to_send = m_files[file_path.filename().string()].NickName;
		}

		client.Send(create_response(200, content, mime, filename_to_send));
	} catch (const std::exception& e) {
		logger.log(Logger::Level::WARNING, std::string("Request error: ") + e.what());
		try {
			client.Send(create_response(404, "Not Found", "text/plain"));
		} catch (...) {
			logger.log(Logger::Level::WARNING, "Failed to send error response");
		}
	}
}

void ChatServer::FileServer() {
	Socket::Initialize();
	ThreadPool pool(config.thread_pool_size);

	try {
		Socket server;
		server.Create(Socket::Protocol::TCP);

		int optval = 1;
			setsockopt(server.get_socket(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

		server.Bind("", config.file_server_port);
		server.Listen();

		const fs::path web_root = "files";
		logger.log(Logger::Level::INFO, "Server started on port " + std::to_string(config.file_server_port));

		while (true) {
			Socket client;
			try {
				client = server.Accept();
			} catch (const SocketException& e) {
				logger.log(Logger::Level::WARNING, std::string("Accept failed: ") + e.what());
				continue;
			}
			pool.enqueue([this, client = std::move(client), web_root]() mutable {
				handle_request(std::move(client), web_root);
			});
		}
	} catch (const SocketException& e) {
		logger.log(Logger::Level::ERROR, std::string("Server error: ") + e.what());
	}
}

int main() {
	setlocale(LC_ALL, "en_US.UTF-8");
	std::cout << "\033[2J\033[1;1H";

	config.load();

	// Auto-create data directories
	if (!fs::exists(config.files_dir)) {
		fs::create_directory(config.files_dir);
	}
	if (!fs::exists(config.logs_dir)) {
		fs::create_directory(config.logs_dir);
	}

	// Auto-create JSON data files with default content
	auto ensure_json_file = [](const std::string& path, const std::string& default_content) {
		if (!fs::exists(path)) {
			std::ofstream f(path);
			if (f.is_open()) {
				f << default_content;
				if (default_content.back() != '\n') f << '\n';
			}
		}
	};
	ensure_json_file(config.users_file, "{}");
	ensure_json_file(config.permission_groups_file, "{}");
	ensure_json_file(config.banned_users_file, "{\"banned_users\":{}}");
	ensure_json_file(config.files_meta_file, "{\"files\":{}}");

	ChatServer server;

	std::thread file_thread(&ChatServer::FileServer, &server);
	file_thread.detach();

	server.run(config.ws_port);
	return 0;
}
