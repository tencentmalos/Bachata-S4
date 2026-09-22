-- Litep query_sql (trace_processor) on a loaded shadPS4 PROF: which lane tracks the frame length.
-- Frames are the frame owner's sceGnmSubmitAndFlipCommandBuffers calls; every lane is clipped to
-- [frame start, frame end) and summed, then correlated with the frame length. Replace the tids:
--   16651 = Guest-19 (frame owner / GNM submit thread), 16652 = shadPS4:GpuComm, 16611 = Guest-1.
-- GPU.GuestCommands is on GPU context tracks with an uncalibrated clock: use it for magnitude only.
WITH f AS (
  SELECT s.ts AS fs, LEAD(s.ts) OVER (ORDER BY s.ts) AS fe
  FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING(utid)
  WHERE t.tid = 16651 AND s.name = 'HLE.sceGnmSubmitAndFlipCommandBuffers'),
fr AS (SELECT fs, fe, fe - fs AS len FROM f WHERE fe IS NOT NULL),
pm AS (SELECT s.ts, s.ts + s.dur AS te FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING(utid)
       WHERE t.tid = 16652 AND s.name = 'PM4.Resume' AND s.dur > 0),
gate AS (SELECT s.ts, s.ts + s.dur AS te FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING(utid)
         WHERE t.tid = 16651 AND s.name IN ('GNM.SubmissionGate', 'HLE.scePthreadCondWait') AND s.dur > 0),
g1w AS (SELECT s.ts, s.ts + s.dur AS te FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING(utid)
        WHERE t.tid = 16611 AND s.name IN ('HLE.scePthreadCondWait', 'HLE.sceKernelWaitSema', 'HLE.shadSyncWait', 'HLE.sceKernelUsleep') AND s.dur > 0),
gpu AS (SELECT ts, ts + dur AS te FROM slice WHERE name = 'GPU.GuestCommands' AND dur > 0),
per AS (
  SELECT fr.fs, fr.len,
    (SELECT COALESCE(SUM(MIN(pm.te, fr.fe) - MAX(pm.ts, fr.fs)), 0) FROM pm WHERE pm.te > fr.fs AND pm.ts < fr.fe) AS pm4,
    (SELECT COALESCE(SUM(MIN(gate.te, fr.fe) - MAX(gate.ts, fr.fs)), 0) FROM gate WHERE gate.te > fr.fs AND gate.ts < fr.fe) AS gate_ms,
    (SELECT COALESCE(SUM(MIN(g1w.te, fr.fe) - MAX(g1w.ts, fr.fs)), 0) FROM g1w WHERE g1w.te > fr.fs AND g1w.ts < fr.fe) AS g1wait,
    (SELECT COALESCE(SUM(MIN(gpu.te, fr.fe) - MAX(gpu.ts, fr.fs)), 0) FROM gpu WHERE gpu.te > fr.fs AND gpu.ts < fr.fe) AS gpu_ms
  FROM fr)
SELECT COUNT(*) AS n,
  ROUND(AVG(len)/1e6, 2) AS frame_ms,
  ROUND(AVG(pm4)/1e6, 2) AS pm4_ms,
  ROUND(AVG(gate_ms)/1e6, 2) AS owner_wait_ms,
  ROUND(AVG(len - g1wait)/1e6, 2) AS guest1_active_ms,
  ROUND(AVG(gpu_ms)/1e6, 2) AS gpu_ms,
  ROUND((AVG(len*pm4) - AVG(len)*AVG(pm4)) / (SQRT(AVG(len*len) - AVG(len)*AVG(len)) * SQRT(AVG(pm4*pm4) - AVG(pm4)*AVG(pm4))), 3) AS corr_pm4,
  ROUND((AVG(len*(len-g1wait)) - AVG(len)*AVG(len-g1wait)) / (SQRT(AVG(len*len) - AVG(len)*AVG(len)) * SQRT(AVG((len-g1wait)*(len-g1wait)) - AVG(len-g1wait)*AVG(len-g1wait))), 3) AS corr_guest1_active,
  ROUND((AVG(len*gpu_ms) - AVG(len)*AVG(gpu_ms)) / (SQRT(AVG(len*len) - AVG(len)*AVG(len)) * SQRT(AVG(gpu_ms*gpu_ms) - AVG(gpu_ms)*AVG(gpu_ms))), 3) AS corr_gpu,
  ROUND((AVG(len*gate_ms) - AVG(len)*AVG(gate_ms)) / (SQRT(AVG(len*len) - AVG(len)*AVG(len)) * SQRT(AVG(gate_ms*gate_ms) - AVG(gate_ms)*AVG(gate_ms))), 3) AS corr_owner_wait
FROM per;
