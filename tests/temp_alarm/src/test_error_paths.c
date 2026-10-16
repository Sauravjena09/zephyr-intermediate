/*
 * temp_alarm - Error-path unit tests (homework skeleton)
 *
 * Task 2: fill in the stubs according to TEST_SPEC.md. Each stub calls
 * ztest_test_skip(), so the binary builds and runs cleanly until the test is
 * implemented.
 *
 * Run:
 *   west twister -T tests/temp_alarm -p native_sim
 */

#include <errno.h>
#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include "sensor_fake.h"
#include "temp_alarm.h"

#define DEFAULT_THRESHOLD 30

/* Same before() hook as in test_basic.c: a clean fake and an initialised module. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	sensor_fake_reset();
	sensor_fake_init();

	zassume_ok(temp_alarm_init(&fake_sensor_dev, DEFAULT_THRESHOLD),
		   "precondition: temp_alarm_init must succeed");
}

/*
 * ============================================================================
 * Test Suite: temp_alarm_sensor_errors
 *
 * Failures from the sensor driver, injected through the fakes.
 * ============================================================================
 */
ZTEST_SUITE(temp_alarm_sensor_errors, NULL, NULL, before, NULL, NULL);

ZTEST(temp_alarm_sensor_errors, test_fetch_error_propagates)
{
	struct temp_alarm_status status;

	/* Make sample_fetch fail with -EIO. */
	fake_sensor_sample_fetch_fake.return_val = -EIO;

	zassert_equal(temp_alarm_check(), -EIO, "check must return -EIO on fetch failure");

	zassert_ok(temp_alarm_get_status(&status), "get_status must succeed");
	zassert_equal(status.last_error, -EIO, "last_error must be -EIO, got %d",
		      status.last_error);

	/* channel_get must NOT be called when fetch fails. */
	zassert_equal(fake_sensor_channel_get_fake.call_count, 0U,
		      "channel_get must not be called after fetch failure, got %u",
		      fake_sensor_channel_get_fake.call_count);
}

ZTEST(temp_alarm_sensor_errors, test_channel_get_error_propagates)
{
	struct temp_alarm_status status;

	/* Clear the custom_fake installed by sensor_fake_init() so return_val is used. */
	fake_sensor_channel_get_fake.custom_fake = NULL;
	fake_sensor_channel_get_fake.return_val = -EIO;

	zassert_equal(temp_alarm_check(), -EIO, "check must return -EIO on channel_get failure");

	zassert_ok(temp_alarm_get_status(&status), "get_status must succeed");
	zassert_equal(status.last_error, -EIO, "last_error must be -EIO, got %d",
		      status.last_error);

	/* sample_fetch was still called exactly once. */
	zassert_equal(fake_sensor_sample_fetch_fake.call_count, 1U,
		      "sample_fetch must be called once, got %u",
		      fake_sensor_sample_fetch_fake.call_count);
}

ZTEST(temp_alarm_sensor_errors, test_fail_then_recover)
{
	struct temp_alarm_status status;

	/* First fetch fails, second succeeds. */
	int fetch_returns[] = {-EIO, 0};

	fake_sensor_sample_fetch_fake.return_val_seq       = fetch_returns;
	fake_sensor_sample_fetch_fake.return_val_seq_len   = 2;

	/* First check — must fail. */
	sensor_fake_set_temperature(20);
	zassert_equal(temp_alarm_check(), -EIO, "first check must return -EIO");

	/* Second check — must succeed and reflect the temperature. */
	zassert_ok(temp_alarm_check(), "second check must succeed");

	zassert_ok(temp_alarm_get_status(&status), "get_status must succeed");
	zassert_equal(status.last_temp, 20, "last_temp must be 20, got %d", status.last_temp);
	zassert_false(status.is_alarming, "is_alarming must be false (20 < 30)");
	/* last_error is kept until a reset — the -EIO from the first check stays. */
	zassert_equal(status.last_error, -EIO, "last_error must still be -EIO, got %d",
		      status.last_error);
}

/*
 * ============================================================================
 * Test Suite: temp_alarm_invalid_input
 *
 * Arguments the public API must reject.
 * ============================================================================
 */
ZTEST_SUITE(temp_alarm_invalid_input, NULL, NULL, before, NULL, NULL);

ZTEST(temp_alarm_invalid_input, test_get_status_null)
{
	/* A NULL status pointer must be rejected with -EINVAL. */
	zassert_equal(temp_alarm_get_status(NULL), -EINVAL,
		      "get_status(NULL) must return -EINVAL");
}

ZTEST(temp_alarm_invalid_input, test_init_without_device)
{
	/* NULL sensor device is rejected with -ENODEV. */
	zassert_equal(temp_alarm_init(NULL, DEFAULT_THRESHOLD), -ENODEV,
		      "init(NULL, threshold) must return -ENODEV");
}

ZTEST(temp_alarm_invalid_input, test_init_threshold_out_of_range)
{
	/* Both -1 and 101 are outside [TEMP_ALARM_THRESHOLD_MIN, TEMP_ALARM_THRESHOLD_MAX]. */
	zassert_equal(temp_alarm_init(&fake_sensor_dev, -1), -EINVAL,
		      "init with threshold -1 must return -EINVAL");
	zassert_equal(temp_alarm_init(&fake_sensor_dev, 101), -EINVAL,
		      "init with threshold 101 must return -EINVAL");
}

ZTEST(temp_alarm_invalid_input, test_set_threshold_out_of_range)
{
	struct temp_alarm_status status;

	/* Both -1 and 101 must be rejected — and the old threshold (30) must stay. */
	zassert_equal(temp_alarm_set_threshold(-1), -EINVAL,
		      "set_threshold(-1) must return -EINVAL");
	zassert_equal(temp_alarm_set_threshold(101), -EINVAL,
		      "set_threshold(101) must return -EINVAL");

	/* Verify the threshold is still 30: a 30 C reading must NOT alarm. */
	sensor_fake_set_temperature(30);
	zassert_ok(temp_alarm_check(), "check at 30 C must succeed");
	zassert_ok(temp_alarm_get_status(&status), "get_status must succeed");
	zassert_false(status.is_alarming,
		      "is_alarming must be false — old 30 C threshold is still in force");
}
