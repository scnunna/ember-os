package com.nunna.embercompanion.android

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.nunna.embercompanion.link.Bond
import com.nunna.embercompanion.link.LinkController
import com.nunna.embercompanion.link.LinkGuard
import com.nunna.embercompanion.link.LinkIssue
import com.nunna.embercompanion.link.LinkTransport
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.UUID

/**
 * BLE glue. Security rules (EMBER_LINK v1.2):
 *  - the watch needs LE Secure Connections bonding with Passkey Entry; we start it with createBond() and Android shows
 *    its own "type the code" dialog;
 *  - nothing is subscribed to or written until the device is BONDED;
 *  - background reconnects happen only for the stored, bonded watch. An unbonded watch is only paired when the user taps Pair;
 *  - authentication errors on a bonded watch mean the watch deleted its bond ("Pair new phone"): we stop and tell the user.
 * All protocol logic lives in [LinkController].
 */
@SuppressLint("MissingPermission")
class EmberGatt(private val ctx: Context, private val link: () -> LinkController) : LinkTransport {
    companion object {
        val SERVICE: UUID = UUID.fromString("eb3f0001-6d62-4f53-8a1e-656d6265720a")
        val CMD: UUID = UUID.fromString("eb3f0002-6d62-4f53-8a1e-656d6265720a")
        val EVT: UUID = UUID.fromString("eb3f0003-6d62-4f53-8a1e-656d6265720a")
        val AUDIO: UUID = UUID.fromString("eb3f0004-6d62-4f53-8a1e-656d6265720a")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val TAG = "EmberGatt"
        val BACKOFF_MS = longArrayOf(1_000, 2_000, 4_000, 8_000, 15_000, 30_000, 60_000)
    }

    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var cmd: BluetoothGattCharacteristic? = null
    private var address: String? = null
    private var wanted = false
    private var userPairing = false
    private var attempt = 0
    private var mtu = 23
    private val queue = ArrayDeque<ByteArray>()
    private var writing = false
    private var subscribeStep = 0
    private var lastAuthError = false
    private var bondFailed = false
    private var receiverOn = false

    private val _bond = MutableStateFlow(Bond.NONE)
    val bond: StateFlow<Bond> = _bond
    private val _issue = MutableStateFlow(LinkIssue.NONE)
    val issue: StateFlow<LinkIssue> = _issue

    /** Last human-readable problem, or null. */
    @Volatile var lastError: String? = null
        private set

    private fun adapter(): BluetoothAdapter? = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private fun device(): BluetoothDevice? = address?.let { runCatching { adapter()?.getRemoteDevice(it) }.getOrNull() }

    private fun bondOf(d: BluetoothDevice?) = when (d?.bondState) {
        BluetoothDevice.BOND_BONDED -> Bond.BONDED
        BluetoothDevice.BOND_BONDING -> Bond.BONDING
        else -> Bond.NONE
    }

    private fun refreshIssue() {
        _bond.value = bondOf(device())
        _issue.value = LinkGuard.classify(_bond.value, lastAuthError, userPairing, bondFailed)
        when (_issue.value) {
            LinkIssue.BOND_LOST -> lastError = Errors.REPAIR
            LinkIssue.PAIRING_FAILED -> lastError = Errors.PAIR_FAILED
            LinkIssue.NEEDS_PAIRING -> lastError = Errors.NEEDS_PAIRING
            else -> {}
        }
    }

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val d: BluetoothDevice? = if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            else @Suppress("DEPRECATION") i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            if (d?.address?.equals(address, true) != true) return
            val state = i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)
            val prev = i.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, BluetoothDevice.ERROR)
            main.post {
                when (state) {
                    BluetoothDevice.BOND_BONDED -> {
                        bondFailed = false; lastAuthError = false; lastError = null
                        refreshIssue()
                        val g = gatt
                        if (g != null && cmd != null) { subscribeStep = 0; subscribeNext(g) }
                    }
                    BluetoothDevice.BOND_NONE -> {
                        if (prev == BluetoothDevice.BOND_BONDING) bondFailed = true
                        refreshIssue()
                    }
                    else -> refreshIssue()
                }
            }
        }
    }

    /**
     * [userInitiated] = the user tapped Pair/Connect. Only then is a new bond started; background restarts of the
     * service (boot, reconnect) never open pairing dialogs.
     */
    fun start(addr: String, userInitiated: Boolean = false) {
        main.post {
            address = addr
            wanted = true
            userPairing = userInitiated
            attempt = 0
            bondFailed = false
            lastAuthError = false
            if (!receiverOn) {
                ctx.registerReceiver(bondReceiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
                receiverOn = true
            }
            refreshIssue()
            connectNow()
        }
    }

    fun stop() {
        main.post {
            wanted = false
            main.removeCallbacksAndMessages(null)
            if (receiverOn) { runCatching { ctx.unregisterReceiver(bondReceiver) }; receiverOn = false }
            closeGatt()
            link().onDisconnected()
        }
    }

    /** Best effort: removes Android's side of the bond so a fresh pairing can start (needed after "Pair new phone" on the watch). */
    fun forgetBond() {
        val d = device() ?: return
        runCatching { BluetoothDevice::class.java.getMethod("removeBond").invoke(d) }
    }

    private fun connectNow() {
        if (!wanted) return
        val a = adapter()
        if (a == null || !a.isEnabled) {
            lastError = Errors.BT_OFF
            scheduleReconnect()
            return
        }
        val dev = device() ?: return
        val bond = bondOf(dev)
        // Background reconnects only for the bonded, stored watch. Anything else waits for the user.
        if (!userPairing && !LinkGuard.mayAutoReconnect(address, AppGraph.prefs.watchAddress, bond)) {
            refreshIssue()
            wanted = false
            return
        }
        closeGatt()
        link().onConnecting()
        try {
            gatt = dev.connectGatt(ctx, false, callback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: Exception) {
            Log.w(TAG, "connect failed", e)
            lastError = Errors.LINK_LOST
            scheduleReconnect()
        }
    }

    private fun closeGatt() {
        queue.clear(); writing = false; cmd = null
        gatt?.let { runCatching { it.disconnect() }; runCatching { it.close() } }
        gatt = null
    }

    private fun scheduleReconnect() {
        if (!wanted) return
        val delay = BACKOFF_MS[minOf(attempt, BACKOFF_MS.size - 1)]
        attempt++
        main.postDelayed({ connectNow() }, delay)
    }

    override fun write(packets: List<ByteArray>) {
        main.post {
            if (!LinkGuard.mayTalk(_bond.value, gatt != null)) return@post // never send before bonded
            queue.addAll(packets)
            pump()
        }
    }

    private fun pump() {
        if (writing) return
        val g = gatt ?: return
        val c = cmd ?: return
        val next = queue.removeFirstOrNull() ?: return
        writing = true
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, next, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION") run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                c.value = next
                g.writeCharacteristic(c)
            }
        }
        if (!ok) { writing = false; Log.w(TAG, "write rejected") }
    }

    /** The watch said "insufficient authentication/encryption". With a bond that means it forgot us; without one, Android starts pairing. */
    private fun onAuthError() {
        lastAuthError = true
        refreshIssue()
        if (_bond.value == Bond.BONDED) {
            // The watch deleted its bond. Reconnecting would only fail again, so stop and tell the user.
            wanted = false
            link().onDisconnected()
            closeGatt()
        }
    }

    private fun startBondingIfNeeded(g: BluetoothGatt): Boolean {
        when (bondOf(g.device)) {
            Bond.BONDED -> return true
            Bond.BONDING -> { refreshIssue(); return false }
            Bond.NONE -> {
                if (!userPairing) { refreshIssue(); wanted = false; closeGatt(); return false }
                refreshIssue()
                if (!g.device.createBond()) { bondFailed = true; refreshIssue() }
                return false
            }
        }
    }

    private fun subscribeNext(g: BluetoothGatt) {
        if (bondOf(g.device) != Bond.BONDED) return // never subscribe before the bond exists
        val svc = g.getService(SERVICE) ?: run {
            lastError = Errors.BT_NO_WATCH
            closeGatt(); scheduleReconnect(); return
        }
        val uuid = when (subscribeStep) { 0 -> EVT; 1 -> AUDIO; else -> null }
        if (uuid == null) {
            attempt = 0
            lastError = null
            lastAuthError = false
            refreshIssue()
            link().onReady(mtu, secure = true)
            return
        }
        val ch = svc.getCharacteristic(uuid)
        if (ch == null) { subscribeStep++; subscribeNext(g); return }
        g.setCharacteristicNotification(ch, true)
        val d = ch.getDescriptor(CCCD)
        if (d == null) { subscribeStep++; subscribeNext(g); return }
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION") run { d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; g.writeDescriptor(d) }
        }
        if (!ok) { Log.w(TAG, "cccd write rejected"); lastError = Errors.LINK_LOST; closeGatt(); scheduleReconnect() }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post {
                if (g !== gatt) return@post
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    mtu = 23
                    if (!g.requestMtu(247)) g.discoverServices()
                } else {
                    link().onDisconnected()
                    if (LinkGuard.isAuthError(status)) onAuthError() else lastError = Errors.LINK_LOST
                    closeGatt()
                    if (wanted) scheduleReconnect()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            main.post {
                if (g !== gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                runCatching { g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED) }
                g.discoverServices()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (g !== gatt) return@post
                val svc = g.getService(SERVICE)
                cmd = svc?.getCharacteristic(CMD)
                if (status != BluetoothGatt.GATT_SUCCESS || svc == null || cmd == null) {
                    lastError = Errors.BT_NO_WATCH
                    closeGatt(); scheduleReconnect(); return@post
                }
                subscribeStep = 0
                if (startBondingIfNeeded(g)) subscribeNext(g)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (g !== gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) { subscribeStep++; subscribeNext(g) }
                else if (LinkGuard.isAuthError(status)) onAuthError()
                else { lastError = Errors.LINK_LOST; closeGatt(); scheduleReconnect() }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            main.post {
                writing = false
                if (LinkGuard.isAuthError(status)) { queue.clear(); onAuthError(); return@post }
                if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "write status $status")
                pump()
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            deliver(c.uuid, value)
        }

        @Deprecated("Android 12/12L callback")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) @Suppress("DEPRECATION") deliver(c.uuid, c.value ?: return)
        }
    }

    private fun deliver(uuid: UUID, value: ByteArray) {
        if (_bond.value != Bond.BONDED) return
        when (uuid) {
            EVT -> link().onEvtPacket(value)
            AUDIO -> link().onAudioPacket(value)
        }
    }
}
