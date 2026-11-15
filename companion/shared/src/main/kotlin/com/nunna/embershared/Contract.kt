package com.nunna.embershared

/** The contract between Ember Companion (no internet) and Ember Gateway (the only app with internet). */
object Contract {
    const val COMPANION_PACKAGE = "com.nunna.embercompanion"
    const val GATEWAY_PACKAGE = "com.nunna.embergateway"

    /** Defined by the Gateway with protectionLevel="signature": only apps signed with the same key can use it. */
    const val PERMISSION = "com.nunna.embergateway.permission.USE"
    const val SERVICE_CLASS = "com.nunna.embergateway.GatewayService"
    const val APPROVALS_ACTIVITY = "com.nunna.embergateway.MainActivity"
    const val AUTHORITY = "com.nunna.embergateway.files"

    // Messenger "what" codes, companion -> gateway
    const val MSG_FETCH_WATCH = 1
    const val MSG_DOWNLOAD_MODEL = 2
    const val MSG_HTTP = 3
    const val MSG_CANCEL = 4

    // gateway -> companion
    const val MSG_PROGRESS = 100
    const val MSG_RESULT = 101
    const val MSG_NEEDS_APPROVAL = 102

    // Bundle keys
    const val K_ID = "id"
    const val K_SSID = "ssid"
    const val K_PASS = "pass"
    const val K_IP = "ip"
    const val K_NAME = "name"
    const val K_TOKEN = "token"
    const val K_URL = "url"
    const val K_LABEL = "label"
    const val K_SIZE = "size"
    const val K_SHA256 = "sha256"
    const val K_METHOD = "method"
    const val K_BODY = "body"
    const val K_PERCENT = "percent"
    const val K_OK = "ok"
    const val K_CODE = "code"
    const val K_MESSAGE = "message"
    const val K_URI = "uri"
    const val K_BYTES = "bytes"

    // Result codes (the companion turns these into plain sentences)
    const val OK = "OK"
    const val DENIED = "DENIED"
    const val EXPIRED = "EXPIRED"
    const val NOT_ALLOWED = "NOT_ALLOWED"
    const val WIFI_JOIN = "WIFI_JOIN"
    const val WATCH_REFUSED = "WATCH_REFUSED" // HTTP 403: wrong token
    const val HTTP_ERROR = "HTTP_ERROR"
    const val SIZE_MISMATCH = "SIZE_MISMATCH"
    const val HASH_MISMATCH = "HASH_MISMATCH"
    const val IO_ERROR = "IO_ERROR"
    const val CANCELLED = "CANCELLED"
}
