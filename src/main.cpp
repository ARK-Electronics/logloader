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
	.default_value(std::string(""))
	.action([](const std::string & value) { return value; });

	program.add_argument("--feedback")
	.help("Additional feedback")
	.default_value(std::string(""))
	.action([](const std::string & value) { return value; });

	program.add_argument("--source")
	.help("Log source (E.g., CI)")
	.default_value(std::string("webui"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--email")
	.help("Your e-mail (to send the upload link)")
	.default_value(std::string(""))
	.action([](const std::string & value) { return value; });

	program.add_argument("--type")
	.help("The upload type (either flightreport or personal)")
	.default_value(std::string("flightreport"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--videoUrl")
	.help("An Url to a video (only used for type flightreport)")
	.default_value(std::string(""))
	.action([](const std::string & value) { return value; });

	program.add_argument("--rating")
	.help("A rating for the flight (only used for type flightreport)")
	.default_value(std::string("notset"))
	.action([](const std::string & value) { return value; });

	program.add_argument("--windSpeed")
	.help("A wind speed category for the flight (only used for flightreport)")
	.default_value(-1)
	.action([](const std::string & value) { return std::stoi(value); });

	program.add_argument("--public")
	.help("Whether the log is uploaded as public (only used for flightreport)")
	.default_value(true)
	.implicit_value(true);

	program.add_argument("files")
	.help("ULog file(s) to upload")
	.remaining();

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

	auto files = program.get<std::vector<std::string>>("files");

	if (files.empty()) {
		std::cerr << "No ULog files specified for upload." << std::endl;
		return 1;
	}

	httplib::Client cli("https://logs.px4.io");

	for (const auto& file_path : files) {
		if (!quiet) {
			std::cout << "Uploading " << file_path << "..." << std::endl;
		}

		std::ifstream file(file_path, std::ios::binary);

		if (!file) {
			std::cerr << "Could not open file " << file_path << std::endl;
			continue;
		}

		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		httplib::MultipartFormDataItems items = {
			{"type", program.get<std::string>("--type"), "", ""},
			{"description", description, "", ""},
			// Add other form fields here
			{"file", content, file_path, "application/octet-stream"}
		};
		auto res = cli.Post("/upload", items);

		if (res && res->status == 302) { // Assuming 302 as a successful upload indicator
			std::cout << "Uploaded successfully. URL: " << res->get_header_value("Location") << std::endl;

		} else {
			std::cerr << "Failed to upload " << file_path << std::endl;
		}
	}

	return 0;
}
