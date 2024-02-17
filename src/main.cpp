#include <iostream>
#include <fstream>
#include <sstream>
#include <httplib.h>
#include <argparse/argparse.hpp>

int main(int argc, char* argv[])
{
	argparse::ArgumentParser program("upload_log");

	program.add_argument("--quiet")
	.default_value(false)
	.implicit_value(true)
	.help("Quiet mode: do not ask for values which were not provided as parameters");

	program.add_argument("--description")
	.help("Log description")
	.default_value(std::string("no description provided"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--feedback")
	.help("Additional feedback")
	.default_value(std::string("no feedback provided"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--email")
	.help("Your e-mail (to send the upload link)")
	.default_value(std::string("dahl.jakejacob@gmail.com"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--public")
	.help("Whether the log is uploaded as public (only used for flightreport)")
	.default_value(true)
	.implicit_value(true);

	program.add_argument("file")
	.help("ULog file to upload")
	.default_value(std::string("test.ulg"));

	try {
		program.parse_args(argc, argv);

	} catch (const std::runtime_error& err) {
		std::cerr << err.what() << std::endl;
		std::cerr << program;
		return 1;
	}

	bool quiet = program.get<bool>("--quiet");
	std::string description = program.get<std::string>("--description");
	// Continue for other arguments

	std::string file_path = program.get<std::string>("file");

	if (file_path.empty()) {
		std::cerr << "No ULog file specified for upload." << std::endl;
		return 1;
	}

	httplib::Client cli("http://logs.px4.io");

	if (!quiet) {
		std::cout << "Uploading " << file_path << "..." << std::endl;
	}

	std::ifstream file(file_path, std::ios::binary);

	if (!file) {
		std::cerr << "Could not open file " << file_path << std::endl;
		return 1;
	}

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	httplib::MultipartFormDataItems items = {
		{"type", "flightreport", "", ""},
		{"description", description, "", ""},
		{"feedback", program.get<std::string>("--feedback"), "", ""},
		{"email", program.get<std::string>("--email"), "", ""},
		{"public", program.get<bool>("--public") ? "true" : "false", "", ""},
		{"file", content, file_path, "application/octet-stream"}
	};
	auto res = cli.Post("/upload", items);

	if (res && res->status == 302) { // Assuming 302 as a successful upload indicator
		std::cout << "Uploaded successfully. URL: " << res->get_header_value("Location") << std::endl;

	} else {
		std::cerr << "Failed to upload " << file_path << std::endl;
	}

	return 0;
}
