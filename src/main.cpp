#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <argparse/argparse.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

int main(int argc, char* argv[])
{
	argparse::ArgumentParser parser("logloader");
	parser.add_argument("--email").help("Your e-mail (to send the upload link)").default_value(std::string("dahl.jakejacob@gmail.com"));
	parser.add_argument("file").help("ULog file to upload").required();

	try {
		parser.parse_args(argc, argv);

	} catch (const std::runtime_error& err) {
		std::cerr << err.what() << std::endl;
		std::cerr << parser;
		return 1;
	}

	std::string file_path = parser.get<std::string>("file");
	std::string email = parser.get("--email");

	httplib::MultipartFormDataItems items = {
		{"type", "personal", "", ""},
		{"description", "Auto Log Upload", "", ""},
		{"feedback", "hmmm", "", ""},
		{"source", "auto", "", ""},
		{"videoUrl", "", "", ""},
		{"rating", "", "", ""},
		{"windSpeed", "", "", ""},
		{"public", "false", "", ""}
	};

	std::ifstream file(file_path, std::ios::binary);

	if (!file) {
		std::cerr << "Could not open file " << file_path << std::endl;
		return 1;
	}

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	items.push_back({"email", email, "", ""});
	items.push_back({"filearg", content, file_path, "application/octet-stream"});

	std::cout << "Uploading..." << std::endl;
	httplib::SSLClient cli("logs.px4.io");

	auto res = cli.Post("/upload", items);

	if (res && res->status == 302) {
		std::cout << "Uploaded successfully. URL: " << res->get_header_value("Location") << std::endl;

	} else {
		std::cerr << "Failed to upload " << file_path << ". Status: " << (res ? std::to_string(res->status) : "No response") << std::endl;
	}

	return 0;
}
