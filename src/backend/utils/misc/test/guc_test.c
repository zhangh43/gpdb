#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"

#include "../guc.c"

static int guc_array_compare(const void *a, const void *b)
{
	const char *namea = *(const char **)a;
	const char *nameb = *(const char **)b;

	return guc_name_compare(namea, nameb);
}

static const char *sync_guc_names_array[] =
{
#include "utils/sync_guc_name.h"
};

static const char *unsync_guc_names_array[] =
{
#include "utils/unsync_guc_name.h"
};
extern int sync_guc_num;
extern int unsync_guc_num;
void init()
{
	sync_guc_num = sizeof(sync_guc_names_array)/ sizeof(char *);
	unsync_guc_num = sizeof(unsync_guc_names_array)/ sizeof(char *);
	qsort((void *) sync_guc_names_array, sync_guc_num,
			sizeof(char *), guc_array_compare);

	qsort((void *) unsync_guc_names_array, unsync_guc_num,
			sizeof(char *), guc_array_compare);
}

static void assert_guc(struct config_generic *conf)
{
	char *res = (char *) bsearch((void *) &conf->name,
			(void *) sync_guc_names_array,
			sync_guc_num,
			sizeof(char *),
			guc_array_compare);
	if (!res)
	{
		char *res = (char *) bsearch((void *) &conf->name,
				(void *) unsync_guc_names_array,
				unsync_guc_num,
				sizeof(char *),
				guc_array_compare);
		assert_true(res);
	}
}

static void
test_bool_guc_coverage(void **state)
{
	for (int i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_generic *conf = (struct config_generic *)&ConfigureNamesBool[i];
		assert_guc(conf);
	}

}

static void
test_int_guc_coverage(void **state)
{
	for (int i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_generic *conf = (struct config_generic *)&ConfigureNamesInt[i];
		assert_guc(conf);
	}

}

static void
test_string_guc_coverage(void **state)
{
	for (int i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_generic *conf = (struct config_generic *)&ConfigureNamesString[i];
		assert_guc(conf);
	}

}

static void
test_real_guc_coverage(void **state)
{
	for (int i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_generic *conf = (struct config_generic *)&ConfigureNamesReal[i];
		assert_guc(conf);
	}

}

static void
test_enum_guc_coverage(void **state)
{
	for (int i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_generic *conf = (struct config_generic *)&ConfigureNamesEnum[i];
		assert_guc(conf);
	}

}

int
main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);
	init();

	const UnitTest tests[] = {
		unit_test(test_bool_guc_coverage),
		unit_test(test_int_guc_coverage),
		unit_test(test_real_guc_coverage),
		unit_test(test_string_guc_coverage),
		unit_test(test_enum_guc_coverage)
	};

	return run_tests(tests);
}
