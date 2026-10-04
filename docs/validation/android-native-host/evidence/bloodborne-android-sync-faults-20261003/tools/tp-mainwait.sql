create perfetto table bb_threads as
select t.utid, t.tid, t.name,
       case when t.name in ('Guest-21','Guest-22','Guest-23','Guest-24','Guest-25') then 'w21-25'
            when t.name in ('Guest-32','Guest-33','Guest-34','Guest-35','Guest-36') then 'w32-36'
            when t.name in ('Guest-54','Guest-55','Guest-56','Guest-57','Guest-58','Guest-59') then 'w54-59'
            else t.name end as grp
from thread t join process p using (upid)
where p.name like 'com.shadps4.android%';
create perfetto table main_waits as
select s.state, s.dur, n.waker_utid
from thread_state s
join thread_state n on n.utid = s.utid and n.ts = s.ts + s.dur
where s.state in ('S', 'D') and s.utid in (select utid from bb_threads where name = 'Guest-1');
-- @@ main thread sleep (S) and uninterruptible (D) time by waker group, ms over the trace
select w.state, coalesce(g.grp, '(none)') as waker_group, count(*) as n,
       round(sum(w.dur) / 1e6, 1) as ms, round(avg(w.dur) / 1e3, 1) as avg_us
from main_waits w left join bb_threads g on g.utid = w.waker_utid
group by w.state, g.grp
order by w.state, ms desc limit 20;
