#include <opustags.h>

#include <getopt.h>

#include <memory>

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

/**
 * Parse the command-line arguments.
 *
 * Return EXIT_SUCCESS on success, meaning the parsing succeeded and the program execution may
 * continue. On error, a relevant message is printed on stderr a non-zero exit code is returned.
 *
 * This function does not perform I/O related validations, but checks the consistency of its
 * arguments.
 */
int ot::parse_options(int argc, char** argv, ot::options& opt)
{
	int c;
	while ((c = getopt_long(argc, argv, "ho:i::yd:a:s:DS", getopt_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			opt.print_help = true;
			break;
		case 'o':
			opt.path_out = optarg;
			if (opt.path_out.empty()) {
				fputs("output's file path cannot be empty\n", stderr);
				return EXIT_FAILURE;
			}
			break;
		case 'i':
			opt.inplace = optarg == nullptr ? ".otmp" : optarg;
			if (strcmp(opt.inplace, "") == 0) {
				fputs("the in-place suffix cannot be empty\n", stderr);
				return EXIT_FAILURE;
			}
			break;
		case 'y':
			opt.overwrite = true;
			break;
		case 'd':
			if (strchr(optarg, '=') != nullptr) {
				fprintf(stderr, "invalid field name: '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			opt.to_delete.emplace_back(optarg);
			break;
		case 'a':
		case 's':
			if (strchr(optarg, '=') == NULL) {
				fprintf(stderr, "invalid comment: '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			opt.to_add.emplace_back(optarg);
			if (c == 's')
				opt.to_delete.emplace_back(optarg);
			break;
		case 'S':
			opt.set_all = true;
			/* fall through */
		case 'D':
			opt.delete_all = true;
			break;
		default:
			/* getopt printed a message */
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

/**
 * \todo Use an std::istream or getline. Lift the 16 KiB limitation and whatever's hardcoded here.
 */
std::list<std::string> ot::read_tags(FILE* file)
{
	std::list<std::string> comments;
	auto raw_tags = std::make_unique<char[]>(16383);
	size_t raw_len = fread(raw_tags.get(), 1, 16382, stdin);
	if (raw_len == 16382)
		fputs("warning: truncating comment to 16 KiB\n", stderr);
	raw_tags[raw_len] = '\0';
	size_t field_len = 0;
	bool caught_eq = false;
	char* cursor = raw_tags.get();
	for (size_t i = 0; i <= raw_len; ++i) {
		if (raw_tags[i] == '\n' || raw_tags[i] == '\0') {
			raw_tags[i] = '\0';
			if (field_len == 0)
				continue;
			if (caught_eq)
				comments.emplace_back(cursor);
			else
				fputs("warning: skipping malformed tag\n", stderr);
			cursor = raw_tags.get() + i + 1;
			field_len = 0;
			caught_eq = false;
			continue;
		}
		if (raw_tags[i] == '=')
			caught_eq = true;
		++field_len;
	}
	return comments;
}