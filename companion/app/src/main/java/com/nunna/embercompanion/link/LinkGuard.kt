package com.nunna.embercompanion.link

/** Bond state of the watch as Android reports it. */
enum class Bond { NONE, BONDING, BONDED }

/** What the user needs to be told or do when the secure link is not working. */
enum class LinkIssue { NONE, NEEDS_PAIRING, TYPE_CODE, BOND_LOST, PAIRING_FAILED }

/**
 * Pure rules for Ember Link security (EMBER_LINK v1.2): bonded + encrypted before anything is sent, and background
 * reconnects only for the one bonded address.
 */
object LinkGuard {
    const val GATT_INSUFFICIENT_AUTHENTICATION = 5
    const val GATT_INSUFFICIENT_ENCRYPTION = 15

    fun isAuthError(gattStatus: Int) = gattStatus == GATT_INSUFFICIENT_AUTHENTICATION || gattStatus == GATT_INSUFFICIENT_ENCRYPTION

    /** Only talk once bonded. (An encrypted link is proven by the watch accepting the subscribe writes.) */
    fun mayTalk(bond: Bond, connected: Boolean) = connected && bond == Bond.BONDED

    /** Background reconnect is for the stored, bonded watch only; unbonded watches need the user to tap Pair. */
    fun mayAutoReconnect(target: String?, stored: String?, bond: Bond) =
        target != null && stored != null && target.equals(stored, true) && bond == Bond.BONDED

    /** A connection to a bonded device that gets auth errors means the watch forgot us ("Pair new phone" on the watch). */
    fun classify(bond: Bond, lastAuthError: Boolean, userStartedPairing: Boolean, bondJustFailed: Boolean): LinkIssue = when {
        bond == Bond.BONDING -> LinkIssue.TYPE_CODE
        bondJustFailed -> LinkIssue.PAIRING_FAILED
        bond == Bond.BONDED && lastAuthError -> LinkIssue.BOND_LOST
        bond == Bond.NONE && !userStartedPairing -> LinkIssue.NEEDS_PAIRING
        else -> LinkIssue.NONE
    }
}
