
#include "self_test.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char buffer[192];

    self_test_device_t device = {
        "MPU6050",
        SELF_TEST_FAIL,
        DEV_ERR_NACK,
        NULL
    };

    assert(SelfTest_FormatLine(
        buffer, sizeof(buffer), &device
    ));

    assert(strcmp(
        buffer,
        "ST,MPU6050,FAIL,err=nack"
    ) == 0);

    device.state = SELF_TEST_OK;
    device.error = DEV_OK;

    assert(SelfTest_FormatLine(
        buffer, sizeof(buffer), &device
    ));

    assert(strcmp(
        buffer,
        "ST,MPU6050,OK"
    ) == 0);

    self_test_summary_t summary = {2, 0, 1, 0};

    assert(SelfTest_FormatSummary(
        buffer, sizeof(buffer), &summary
    ));

    assert(strcmp(
        buffer,
        "ST,SUMMARY,FAIL,ok=2,warn=0,fail=1,skip=0"
    ) == 0);

    puts("Self-test formatting: ALL TESTS PASSED");

    return 0;
}
