package com.nunna.embercompanion.protocol

import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/** Plain description of one Health Connect StepsRecord, so the mapping can be tested on the JVM. */
data class StepsRecordSpec(
    val clientRecordId: String,
    val start: Instant,
    val end: Instant,
    val count: Long,
    val zoneOffsetSeconds: Int,
    val version: Long,
)

object StepsMapping {
    /** `20261114` -> 2026-11-14, or null if the number is not a real date. */
    fun parseDate(d: Int): LocalDate? = try {
        LocalDate.of(d / 10000, (d / 100) % 100, d % 100)
    } catch (e: Exception) {
        null
    }

    /**
     * One record per day: from local midnight to the next midnight (or [now] for today, because Health Connect does
     * not accept records that end in the future). The clientRecordId is per date so re-syncing a day updates it
     * instead of adding a duplicate; [now] is used as the record version so the newer count wins.
     */
    fun toRecords(days: List<Message.StepDay>, zone: ZoneId, now: Instant): List<StepsRecordSpec> =
        days.mapNotNull { d ->
            val date = parseDate(d.date) ?: return@mapNotNull null
            if (d.steps <= 0) return@mapNotNull null
            val start = date.atStartOfDay(zone).toInstant()
            val nextMidnight = date.plusDays(1).atStartOfDay(zone).toInstant()
            val end = if (nextMidnight.isAfter(now)) now else nextMidnight
            if (!end.isAfter(start)) return@mapNotNull null
            StepsRecordSpec(
                clientRecordId = "ember-steps-${d.date}",
                start = start, end = end, count = d.steps.toLong(),
                zoneOffsetSeconds = zone.rules.getOffset(start).totalSeconds,
                version = now.toEpochMilli(),
            )
        }
}
