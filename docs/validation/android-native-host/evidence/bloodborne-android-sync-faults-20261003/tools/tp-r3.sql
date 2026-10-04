create perfetto table bb_threads as
select t.utid, t.tid, t.name,
       case when t.name in ('Guest-21','Guest-22','Guest-23','Guest-24','Guest-25') then 'w21-25'
            when t.name in ('Guest-32','Guest-33','Guest-34','Guest-35','Guest-36') then 'w32-36'
            when t.name in ('Guest-54','Guest-55','Guest-56','Guest-57','Guest-58','Guest-59') then 'w54-59'
            when t.name in ('Guest-1','Guest-17','Guest-20') then t.name
            when t.name like 'Guest-%' then 'guest-other'
            else t.name end as grp
from thread t join process p using (upid)
where p.name like 'com.shadps4.android%';
create perfetto table trace_bounds_ms as
select (select max(ts + dur) from sched_slice) - (select min(ts) from sched_slice) as span;
-- @@ thread state totals per group, ms over the trace (Running from sched; R = runnable, S sleep, D uninterruptible)
select g.grp, ts.state, round(sum(ts.dur) / 1e6, 1) as ms, count(*) as n
from thread_state ts join bb_threads g using (utid)
where g.grp in ('Guest-1', 'Guest-17', 'Guest-20', 'w21-25', 'w32-36', 'w54-59', 'shadPS4:GpuComm', 'shadPS4:VkRecor', 'shadPS4:Waker')
group by g.grp, ts.state
having ms > 5
order by g.grp, ms desc;
-- @@ running time by cluster (little 0-2, mid 3-6, big 7), ms over the trace
select g.grp,
       round(sum(case when ss.cpu <= 2 then ss.dur else 0 end) / 1e6, 1) as little_ms,
       round(sum(case when ss.cpu between 3 and 6 then ss.dur else 0 end) / 1e6, 1) as mid_ms,
       round(sum(case when ss.cpu = 7 then ss.dur else 0 end) / 1e6, 1) as big_ms
from sched_slice ss join bb_threads g using (utid)
group by g.grp
having little_ms + mid_ms + big_ms > 20
order by little_ms + mid_ms + big_ms desc;
-- @@ Guest-1 sleeps (S/D) by waker group
select ts.state, coalesce(w.grp, '(none)') as waker_group, count(*) as n,
       round(sum(ts.dur) / 1e6, 1) as ms, round(avg(ts.dur) / 1e3, 1) as avg_us
from thread_state ts
join bb_threads g on g.utid = ts.utid
left join thread_state n on n.utid = ts.utid and n.ts = ts.ts + ts.dur
left join bb_threads w on w.utid = n.waker_utid
where ts.state in ('S', 'D') and g.name = 'Guest-1'
group by ts.state, w.grp
order by ts.state, ms desc limit 15;
-- @@ CPU utilisation per cpu over the trace (all processes), percent busy
select cpu, round(100.0 * sum(dur) / (select span from trace_bounds_ms), 1) as busy_pct
from sched_slice where utid != 0
group by cpu order by cpu;
