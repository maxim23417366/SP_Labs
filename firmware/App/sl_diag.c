/**
  ******************************************************************************
  * @file    sl_diag.c
  * @brief   ЛР3 (I10): лічильники діагностики модулів SmartLock у STATS.
  *
  *          Єдина прошивка: STATS (I03) показує не лише час задач, а й те, чи
  *          модулі K02–K08 / I09 нічого не губили (docs/lab_3/fsm-access-control.md,
  *          «додати при I10»). Сильна SmartLock_PrintDiag() (smartlock_hooks.h)
  *          замінює слабку зі smartlock_hooks_weak.c. Ім'я sl_* — щоб бінарник
  *          слабких реалізацій (tests/Makefile, test_hooks_weak) її не лінкував.
  *
  *          Формат — по рядку на модуль, «ключ=значення» (як EVQ / UART у STATS):
  *            DIAG name=fsm state=<стан> ignored=<n> cam_rejected=<n>
  *            DIAG name=buttons dropped=<n> late=<n> key_lost=<n>
  *              (late — натискання коду, які додала EXTI-черга під час довгих операцій,
  *               key_lost — відкинуті через повну чергу; issue #51)
  *            DIAG name=security mpu_ok=0|1 mpu_err=<n> mpu_zero=<n> lost=<n> door_retries=<n>
  *            DIAG name=env dht_err=<n> bmp_err=<n> warnings=<n> post_failed=<n>
  *            DIAG name=journal pending=<n> max_depth=<n> stored=<n> unsaved=<n> dropped=<n> last_seq=<n>
  *            DIAG name=camlink requested=<n> rejected=<n> ok=<n> failed=<n> timeouts=<n> unknown=<n> no_id=<n> pending=<n>
  *            DIAG name=camera accepted=<n> rejected=<n> stored=<n> capture_err=<n> store_err=<n> pending=0|1
  *          Нормальний стенд: усі dropped / lost / unsaved / post_failed = 0.
  ******************************************************************************
  */
#include "smartlock_hooks.h"

#include <stdio.h>

#include "camera.h"
#include "platform.h"
#include "sl_buttons.h"
#include "sl_camera_link.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"

/* Без camera.c (тести K08 зі слабкою заглушкою камери) — лише ознака відсутності.
 * GCC не підставляє тіло слабкої функції у виклик, тож сильна з camera.c працює. */
__attribute__((weak)) void Camera_PrintDiag(void)
{
    printf("DIAG name=camera present=0\r\n");
}

void SmartLock_PrintDiag(void)
{
    const sl_camlink_stats_t *cl = SmartLock_CameraLinkStats();

    printf("DIAG name=fsm state=%s ignored=%lu cam_rejected=%lu\r\n",
           SmartLock_FsmStateName(SmartLock_FsmState()),
           (unsigned long)SmartLock_FsmIgnored(), (unsigned long)SmartLock_FsmCameraRejected());
    printf("DIAG name=buttons dropped=%lu late=%lu key_lost=%lu\r\n",
           (unsigned long)SmartLock_ButtonsDropped(), (unsigned long)SmartLock_ButtonsLate(),
           (unsigned long)Platform_KeyLost());
    printf("DIAG name=security mpu_ok=%u mpu_err=%lu mpu_zero=%lu lost=%lu door_retries=%lu\r\n",
           SlSecurity_MpuHealthy() ? 1U : 0U, (unsigned long)SlSecurity_MpuErrors(),
           (unsigned long)SlSecurity_MpuZeroSamples(), (unsigned long)SlSecurity_EventsLost(),
           (unsigned long)SlSecurity_DoorRetries());
    printf("DIAG name=env dht_err=%lu bmp_err=%lu warnings=%lu post_failed=%lu\r\n",
           (unsigned long)SmartLock_EnvDhtErrors(), (unsigned long)SmartLock_EnvBmpErrors(),
           (unsigned long)SmartLock_EnvWarningsPosted(), (unsigned long)SmartLock_EnvPostFailed());
    printf("DIAG name=journal pending=%u max_depth=%u stored=%lu unsaved=%lu dropped=%lu "
           "last_seq=%lu\r\n",
           SmartLock_JournalPending(), SmartLock_JournalMaxDepth(),
           (unsigned long)SmartLock_JournalStored(), (unsigned long)SmartLock_JournalUnsaved(),
           (unsigned long)SmartLock_JournalDropped(), (unsigned long)SmartLock_JournalLastSeq());
    printf("DIAG name=camlink requested=%lu rejected=%lu ok=%lu failed=%lu timeouts=%lu "
           "unknown=%lu no_id=%lu pending=%u\r\n",
           (unsigned long)cl->requested, (unsigned long)cl->rejected, (unsigned long)cl->ok,
           (unsigned long)cl->failed, (unsigned long)cl->timeouts, (unsigned long)cl->unknown,
           (unsigned long)cl->skipped_no_id, SmartLock_CameraLinkPending());
    Camera_PrintDiag();
}
