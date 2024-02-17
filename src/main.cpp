#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <argparse/argparse.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

int main(int argc, char* argv[])
{
	argparse::ArgumentParser program("upload_log");

	program.add_argument("--quiet")
	.default_value(false)
	.implicit_value(true)
	.help("Quiet mode: do not ask for values which were not provided as parameters");

	program.add_argument("--description")
	.help("Log description")
	.default_value(std::string("This is a test"));

	program.add_argument("--feedback")
	.help("Additional feedback")
	.default_value(std::string("lol"));

	program.add_argument("--source")
	.help("Log source (Eg. CI)")
	.default_value(std::string("webui"));

	program.add_argument("--email")
	.help("Your e-mail (to send the upload link)")
	.default_value(std::string("dahl.jakejacob@gmail.com"));

	program.add_argument("--type")
	.help("The upload type (either flightreport or personal)")
	.default_value(std::string("flightreport"));

	program.add_argument("--videoUrl")
	.help("An URL to a video (only used for type flightreport)")
	.default_value(std::string("none"));

	program.add_argument("--rating")
	.help("A rating for the flight (only used for type flightreport)")
	.default_value(std::string("none"));

	program.add_argument("--windSpeed")
	.help("A wind speed category for the flight (only used for flightreport)")
	.default_value(0)
	.scan<'i', int>();

	program.add_argument("--public")
	.help("Whether the log is uploaded as public (only used for flightreport)")
	.default_value(true)
	.implicit_value(true);

	program.add_argument("file")
	.help("ULog file to upload")
	.required();

	try {
		program.parse_args(argc, argv);

	} catch (const std::runtime_error& err) {
		std::cerr << err.what() << std::endl;
		std::cerr << program;
		return 1;
	}

	std::string file_path = program.get<std::string>("file");
	std::string description = program.get("--description");
	std::string feedback = program.get("--feedback");
	std::string email = program.get("--email");
	std::string source = program.get("--source");
	std::string type = program.get("--type");
	bool is_public = program.get<bool>("--public");
	int windSpeed = program.get<int>("--windSpeed");
	std::string videoUrl = program.get("--videoUrl");
	std::string rating = program.get("--rating");

	httplib::MultipartFormDataItems items = {
		{"type", type, "", ""},
		{"description", description, "", ""},
		{"feedback", feedback, "", ""},
		{"email", email, "", ""},
		{"source", source, "", ""},
		{"videoUrl", videoUrl, "", ""},
		{"rating", rating, "", ""},
		{"windSpeed", std::to_string(windSpeed), "", ""},
		{"public", is_public ? "true" : "false", "", ""}
	};

	std::ifstream file(file_path, std::ios::binary);

	if (!file) {
		std::cerr << "Could not open file " << file_path << std::endl;
		return 1;
	}

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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
