#pragma once

#define IDI_APP_ICON 101

// Control IDs
#define IDC_COMBO_SSID          1001
#define IDC_BTN_SCAN            1002
#define IDC_COMBO_AUTH_MODE     1003
#define IDC_EDIT_USERNAME       1004
#define IDC_EDIT_PASSWORD       1005
#define IDC_CHK_SHOW_PWD        1006
#define IDC_EDIT_DOMAIN_OR_PSK  1007
#define IDC_LBL_DOMAIN_OR_PSK   1008

#define IDC_EDIT_PORTAL_URL     1009
#define IDC_COMBO_PORTAL_METHOD 1010
#define IDC_EDIT_USER_FIELD     1011
#define IDC_EDIT_PASS_FIELD     1012
#define IDC_EDIT_EXTRA_PARAMS   1013

#define IDC_CHK_AUTO_CONNECT    1014
#define IDC_EDIT_INTERVAL       1015
#define IDC_CHK_AUTO_START      1016

#define IDC_BTN_CONNECT         1017
#define IDC_BTN_DISCONNECT      1018
#define IDC_BTN_TEST_NET        1019
#define IDC_BTN_SAVE            1020
#define IDC_BTN_CLEAR_LOG       1021

#define IDC_EDIT_LOG            1022
#define IDC_LBL_STATUS          1023

// System Tray & Custom Messages
#define WM_TRAYICON             (WM_USER + 100)
#define WM_APP_LOG              (WM_USER + 101)
#define WM_APP_STATUS           (WM_USER + 102)
#define WM_APP_SCAN_DONE        (WM_USER + 103)
#define WM_APP_OP_DONE          (WM_USER + 104)

// Tray Menu IDs
#define IDM_TRAY_OPEN           2001
#define IDM_TRAY_CONNECT        2002
#define IDM_TRAY_TOGGLE_AUTO    2003
#define IDM_TRAY_EXIT           2004
