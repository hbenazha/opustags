/**
 * \file src/cli.cc
 * \ingroup cli
 *
 * Provide all the features of the opustags executable from a C++ API. The main point of separating
 * this module from the main one is to allow easy testing.
 *
 * \todo Use a safer temporary file name for in-place editing, like tmpnam.
 * \todo Abort editing with --set-all if one comment is invalid?
 */

#include <config.h>
#include <opustags.h>

#include <getopt.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

using namespace std::literals::string_literals;

static const char help_message[] =
PROJECT_NAME " version " PROJECT_VERSION
R"raw(

Usage: opustags --help
       opustags [OPTIONS] FILE
       opustags OPTIONS FILE -o FILE

Options:
  -h, --help              print this help
  -o, --output FILE       set the output file
  -i, --in-place          overwrite the input file instead of writing a different output file
  -y, --overwrite         overwrite the output file if it already exists
  -a, --add FIELD=VALUE   add a comment
  -d, --delete FIELD      delete all previously existing comments of a specific type
  -D, --delete-all        delete all the previously existing comments
  -s, --set FIELD=VALUE   replace a comment (shorthand for --delete FIELD --add FIELD=VALUE)
  -S, --set-all           replace all the comments with the ones read from standard input

See the man page for extensive documentation.
)raw";

static struct option getopt_options[] = {
	{"help", no_argument, 0, 'h'},
	{"output", required_argument, 0, 'o'},
	{"in-place", optional_argument, 0, 'i'},
	{"overwrite", no_argument, 0, 'y'},
	{"delete", required_argument, 0, 'd'},
	{"add", required_argument, 0, 'a'},
	{"set", required_argument, 0, 's'},
	{"delete-all", no_argument, 0, 'D'},
	{"set-all", no_argument, 0, 'S'},
	{NULL, 0, 0, 0}
};

ot::status ot::parse_options(int argc, char** argv, ot::options& opt)
{
	opt = {};
	if (argc == 1)
		return {st::bad_arguments, "No arguments specified. Use -h for help."};
	bool in_place = false;
	int c;
	optind = 0;
	while ((c = getopt_long(argc, argv, ":ho:iyd:a:s:DS", getopt_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			opt.print_help = true;
			break;
		case 'o':
			if (!opt.path_out.empty())
				return {st::bad_arguments, "Cannot specify --output more than once."};
			opt.path_out = optarg;
			if (opt.path_out.empty())
				return {st::bad_arguments, "Output file path cannot be empty."};
			break;
		case 'i':
			in_place = true;
			break;
		case 'y':
			opt.overwrite = true;
			break;
		case 'd':
			if (strchr(optarg, '=') != nullptr)
				return {st::bad_arguments, "Invalid field name '"s + optarg + "'."};
			opt.to_delete.emplace_back(optarg);
			break;
		case 'a':
		case 's':
			if (strchr(optarg, '=') == NULL)
				return {st::bad_arguments, "Invalid comment '"s + optarg + "'."};
			opt.to_add.emplace_back(optarg);
			if (c == 's')
				opt.to_delete.emplace_back(optarg);
			break;
		case 'S':
			opt.set_all = true;
			break;
		case 'D':
			opt.delete_all = true;
			break;
		case ':':
			return {st::bad_arguments,
			        "Missing value for option '"s + argv[optind - 1] + "'."};
		default:
			return {st::bad_arguments, "Unrecognized option '" +
			        (optopt ? "-"s + static_cast<char>(optopt) : argv[optind - 1]) + "'."};
		}
	}
	if (opt.print_help)
		return st::ok;
	if (optind != argc - 1)
		return {st::bad_arguments, "Exactly one input file must be specified."};
	opt.path_in = argv[optind];
	if (opt.path_in.empty())
		return {st::bad_arguments, "Input file path cannot be empty."};
	if (in_place) {
		if (!opt.path_out.empty())
			return {st::bad_arguments, "Cannot combine --in-place and --output."};
		if (opt.path_in == "-")
			return {st::bad_arguments, "Cannot modify standard input in place."};
		opt.path_out = opt.path_in;
		opt.overwrite = true;
	}
	if (opt.path_in == "-" && opt.set_all)
		return {st::bad_arguments,
		        "Cannot use standard input as input file when --set-all is specified."};
	return st::ok;
}

/**
 * \todo Escape new lines.
 */
void ot::print_comments(const std::list<std::string>& comments, FILE* output)
{
	for (const std::string& comment : comments) {
		fwrite(comment.data(), 1, comment.size(), output);
		puts("");
	}
}

std::list<std::string> ot::read_comments(FILE* input)
{
	std::list<std::string> comments;
	char* line = nullptr;
	size_t buflen = 0;
	ssize_t nread;
	while ((nread = getline(&line, &buflen, input)) != -1) {
		if (nread > 0 && line[nread - 1] == '\n')
			--nread;
		if (nread == 0)
			continue;
		if (memchr(line, '=', nread) == nullptr) {
			fputs("warning: skipping malformed tag\n", stderr);
			continue;
		}
		comments.emplace_back(line, nread);
	}
	free(line);
	return comments;
}

/**
 * Parse the packet as an OpusTags comment header, apply the user's modifications, and write the new
 * packet to the writer.
 */
static ot::status process_tags(const ogg_packet& packet, const ot::options& opt, ot::ogg_writer* writer)
{
	ot::opus_tags tags;
	ot::status rc = ot::parse_tags(packet, tags);
	if (rc != ot::st::ok)
		return rc;

	if (opt.delete_all) {
		tags.comments.clear();
	} else {
		for (const std::string& name : opt.to_delete)
			ot::delete_comments(tags, name.c_str());
	}

	if (opt.set_all)
		tags.comments = ot::read_comments(stdin);
	for (const std::string& comment : opt.to_add)
		tags.comments.emplace_back(comment);

	if (writer) {
		auto packet = ot::render_tags(tags);
		return writer->write_packet(packet);
	} else {
		ot::print_comments(tags.comments, stdout);
		return ot::st::ok;
	}
}

/**
 * Main loop of opustags. Read the packets from the reader, and forwards them to the writer.
 * Transform the OpusTags packet on the fly.
 *
 * The writer is optional. When writer is nullptr, opustags runs in read-only mode.
 */
static ot::status process(ot::ogg_reader& reader, ot::ogg_writer* writer, const ot::options &opt)
{
	int packet_count = 0;
	for (;;) {
		// Read the next page.
		ot::status rc = reader.read_page();
		if (rc == ot::st::end_of_stream)
			break;
		else if (rc != ot::st::ok)
			return rc;
		// Short-circuit when the relevant packets have been read.
		if (packet_count >= 2 && writer) {
			if ((rc = writer->write_page(reader.page)) != ot::st::ok)
				return rc;
			continue;
		}
		auto serialno = ogg_page_serialno(&reader.page);
		if (writer && (rc = writer->prepare_stream(serialno)) != ot::st::ok)
			return rc;
		// Read all the packets.
		for (;;) {
			rc = reader.read_packet();
			if (rc == ot::st::end_of_page)
				break;
			else if (rc != ot::st::ok)
				return rc;
			packet_count++;
			if (packet_count == 1) { // Identification header
				rc = ot::validate_identification_header(reader.packet);
				if (rc != ot::st::ok)
					return rc;
			} else if (packet_count == 2) { // Comment header
				rc = process_tags(reader.packet, opt, writer);
				if (rc != ot::st::ok)
					return rc;
				if (!writer)
					return ot::st::ok; /* nothing else to do */
				else
					continue; /* process_tags wrote the new packet */
			}
			if (writer && (rc = writer->write_packet(reader.packet)) != ot::st::ok)
				return rc;
		}
		// Write the assembled page.
		if (writer && (rc = writer->flush_page()) != ot::st::ok)
			return rc;
	}
	if (packet_count < 2)
		return {ot::st::fatal_error, "Expected at least 2 Ogg packets"};
	return ot::st::ok;
}

ot::status ot::run(const ot::options& opt)
{
	if (opt.print_help) {
		fputs(help_message, stdout);
		return st::ok;
	}

	ot::file input;
	if (opt.path_in == "-")
		input = stdin;
	else if ((input = fopen(opt.path_in.c_str(), "r")) == nullptr)
		return {ot::st::standard_error,
		        "Could not open '" + opt.path_in + "' for reading: " + strerror(errno)};
	ot::ogg_reader reader(input.get());

	/* Read-only mode. */
	if (opt.path_out.empty())
		return process(reader, nullptr, opt);

	/* Read-write mode.
	 *
	 * The output pointer is set to one of:
	 *  - stdout for "-",
	 *  - final_output.get() for special files like /dev/null,
	 *  - temporary_output.get() for regular files.
	 *
	 * We use a temporary output file for the following reasons:
	 *  1. The partial .opus output may be seen by softwares like media players, or through
	 *     inotify for the most attentive process.
	 *  2. If the process crashes badly, or the power cuts off, we don't want to leave a partial
	 *     file at the final location. The temporary file is still going to stay but will have an
	 *     obvious name.
	 *  3. If we're overwriting a regular file, we'd rather avoid wiping its content before we
	 *     even started reading the input file. That way, the original file is always preserved
	 *     on error or crash.
	 *  4. It is necessary for in-place editing. We can't reliably open the same file as both
	 *     input and output.
	 */

	FILE* output = nullptr;
	ot::partial_file temporary_output;
	ot::file final_output;

	ot::status rc = ot::st::ok;
	struct stat output_info;
	if (opt.path_out == "-") {
		output = stdout;
	} else if (stat(opt.path_out.c_str(), &output_info) == 0) {
		/* The output file exists. */
		if (!S_ISREG(output_info.st_mode)) {
			/* Special files are opened for writing directly. */
			if ((final_output = fopen(opt.path_out.c_str(), "w")) == nullptr)
				rc = {ot::st::standard_error,
				      "Could not open '" + opt.path_out + "' for writing: " +
				       strerror(errno)};
			output = final_output.get();
		} else if (opt.overwrite) {
			rc = temporary_output.open(opt.path_out.c_str());
			output = temporary_output.get();
		} else {
			rc = {ot::st::fatal_error,
			      "'" + opt.path_out + "' already exists. Use -y to overwrite."};
		}
	} else if (errno == ENOENT) {
		rc = temporary_output.open(opt.path_out.c_str());
		output = temporary_output.get();
	} else {
		rc = {ot::st::fatal_error,
		      "Could not identify '" + opt.path_in + "': " + strerror(errno)};
	}
	if (rc != ot::st::ok)
		return rc;

	ot::ogg_writer writer(output);
	rc = process(reader, &writer, opt);
	if (rc == ot::st::ok)
		rc = temporary_output.commit();

	return rc;
}
