import datetime

class ThreadStatus(object):
    STATUS_NONE = -1
    STATUS_READY = 0
    STATUS_FETCH_MSG = 1
    STATUS_GET_FIN = 2
    STATUS_RECONNECT = 3
    STATUS_GET_RESP = 4

    def __init__(self, status_code):
        self.status_code = status_code
        self.prior = {}
        self.ts = None

    def add_prior(self, prior_status, handler):
        self.prior[prior_status] = handler

    @staticmethod
    def get_status_code_from_content(content):
        if content.startswith("Start to work"):
            return ThreadStatus.STATUS_READY
        elif content.startswith("Fetch msg"):
            return ThreadStatus.STATUS_FETCH_MSG
        elif content.startswith("Get FIN"):
            return ThreadStatus.STATUS_GET_FIN
        elif content.startswith("Connect succeed"):
            return ThreadStatus.STATUS_RECONNECT
        elif content.startswith("Get resp"):
            return ThreadStatus.STATUS_GET_RESP
        else:
            return ThreadStatus.STATUS_NONE

    def get_time_diff(self, ts_start, ts_end):
        if ts_start > ts_end:
            raise Exception("TS end ahead of start")
        return ts_end - ts_start

    def ready(self, thread, ts):
        self.ts = ts

    def fetch(self, thread, ts):
        self.ts = ts
        last = thread.status_table[self.STATUS_GET_RESP]
        if last.ts is None:
            return
        start = thread.status_table[self.STATUS_READY]
        interval = self.get_time_diff(last.ts, self.ts)
        time_idx = self.get_time_diff(start.ts, self.ts)
        thread.switch_time.append((time_idx, interval))

    def get_fin(self, thread, ts):
        self.ts = ts

    def reconnect(self, thread, ts):
        self.ts = ts
        fetch = thread.status_table[self.STATUS_FETCH_MSG]
        start = thread.status_table[self.STATUS_READY]
        interval = self.get_time_diff(fetch.ts, self.ts)
        time_idx = self.get_time_diff(start.ts, self.ts)
        thread.connect_time.append((time_idx, interval))
        thread.connect_cnt += 1

    def get_resp_fin(self, thread, ts):
        self.ts = ts
        connect = thread.status_table[self.STATUS_RECONNECT]
        start = thread.status_table[self.STATUS_READY]
        interval = self.get_time_diff(connect.ts, self.ts)
        time_idx = self.get_time_diff(start.ts, self.ts)
        thread.send_time.append((time_idx, interval))
        thread.msg_cnt += 1
        self.check_send_spoiler(thread, interval, ts)

    def get_resp(self, thread, ts):
        self.ts = ts
        fetch = thread.status_table[self.STATUS_FETCH_MSG]
        start = thread.status_table[self.STATUS_READY]
        interval = self.get_time_diff(fetch.ts, self.ts)
        time_idx = self.get_time_diff(start.ts, self.ts)
        thread.send_time.append((time_idx, interval))
        thread.msg_cnt += 1
        self.check_send_spoiler(thread, interval, ts)

    def check_send_spoiler(self, thread, interval, ts):
        try:
            _SPOILER_SEND_THREASHOLD_
        except:
            return
        ms = interval.seconds * 1000
        ms += interval.microseconds / 1000
        if ms >= _SPOILER_SEND_THREASHOLD_:
            print "SPOILER {0}, {1}, {2}:{3}:{4}.{5}".format(thread.tid, interval,\
                  ts.hour, ts.minute, ts.second, ts.microsecond/1000)

    def __str__(self):
        if self.status_code == ThreadStatus.STATUS_READY:
            return "ready"
        elif self.status_code == ThreadStatus.STATUS_FETCH_MSG:
            return "fetch_msg"
        elif self.status_code == ThreadStatus.STATUS_GET_FIN:
            return "get_fin"
        elif self.status_code == ThreadStatus.STATUS_RECONNECT:
            return "reconnect"
        elif self.status_code == ThreadStatus.STATUS_GET_RESP:
            return "get_resp"
        elif self.status_code == ThreadStatus.STATUS_NONE:
            return "none"

class SendThread(object):
    def __init__(self, tid):
        self.tid = tid
        self.status_table = self.init_status_table()
        self.last_status = self.status_table[ThreadStatus.STATUS_NONE]
        self.send_time = []
        self.switch_time = []
        self.connect_time = []
        self.msg_cnt = 0
        self.connect_cnt = 0

    def init_status_table(self):
        t = {}
        status_list = [ThreadStatus.STATUS_NONE, ThreadStatus.STATUS_READY,
                       ThreadStatus.STATUS_FETCH_MSG, ThreadStatus.STATUS_GET_FIN,
                       ThreadStatus.STATUS_RECONNECT, ThreadStatus.STATUS_GET_RESP]
        for status in status_list:
            t[status] = ThreadStatus(status)

        t[ThreadStatus.STATUS_READY].add_prior(ThreadStatus.STATUS_NONE,
                                               ThreadStatus.ready)
        t[ThreadStatus.STATUS_FETCH_MSG].add_prior(ThreadStatus.STATUS_READY,
                                                   ThreadStatus.fetch)
        t[ThreadStatus.STATUS_FETCH_MSG].add_prior(ThreadStatus.STATUS_GET_RESP,
                                                   ThreadStatus.fetch)
        t[ThreadStatus.STATUS_GET_FIN].add_prior(ThreadStatus.STATUS_FETCH_MSG,
                                                 ThreadStatus.get_fin)
        t[ThreadStatus.STATUS_RECONNECT].add_prior(ThreadStatus.STATUS_GET_FIN,
                                                   ThreadStatus.reconnect)
        t[ThreadStatus.STATUS_GET_RESP].add_prior(ThreadStatus.STATUS_RECONNECT,
                                                  ThreadStatus.get_resp_fin)
        t[ThreadStatus.STATUS_GET_RESP].add_prior(ThreadStatus.STATUS_FETCH_MSG,
                                                  ThreadStatus.get_resp)
        return t

    def consume_line(self, line):
        ts, content = self.parse_line(line)
        status_code = ThreadStatus.get_status_code_from_content(content)
        if status_code == ThreadStatus.STATUS_NONE:
            return
        last_status_code = self.last_status.status_code
        if last_status_code == ThreadStatus.STATUS_NONE and \
           status_code == ThreadStatus.STATUS_RECONNECT:
           return
        cur_status = self.status_table[status_code]
        valid_prior_status = cur_status.prior.keys()
        if last_status_code not in valid_prior_status:
            raise Exception("Last status {0} not a valid prior for {1}".format(\
                            self.last_status, cur_status))
        cur_status.prior[last_status_code](cur_status, self, ts)
        self.last_status = cur_status

    def parse_line(self, line):
        toks = line.split()
        ts = get_ts(toks[0])
        content = " ".join(toks[3:])
        return ts, content

    def format_time_index(self, time_index):
        seconds = float(time_index.seconds)
        seconds += float(time_index.microseconds) / (1000*1000)
        return seconds

    def format_interval(self, interval):
        ms = interval.seconds * 1000
        ms += interval.microseconds / 1000
        return ms

    def write_send_time_csv(self, fcsv):
        avg_send_time, min_time, max_time = 0, -1, 0
        cnt = 0
        for time_index, interval in self.send_time:
            time_index = self.format_time_index(time_index)
            interval = self.format_interval(interval)
            fcsv.write("{0},{1},{2}\n".format(self.tid, time_index, interval))
            avg_send_time += interval
            cnt += 1
            if min_time == -1:
                min_time = interval
            else:
                min_time = min(min_time, interval)
            max_time = max(max_time, interval)
        if cnt == 0:
            return
        avg_send_time = float(avg_send_time) / cnt
        print "<{0}> Average send time {1:0.1f}, msg count {2}, [{3}, {4}]".format(\
              self.tid, avg_send_time, cnt, min_time, max_time)

    def write_switch_time_csv(self, fcsv):
        avg_switch_time, min_time, max_time = 0, -1, 0
        cnt = 0
        for time_index, interval in self.switch_time:
            time_index = self.format_time_index(time_index)
            interval = self.format_interval(interval)
            fcsv.write("{0},{1},{2}\n".format(self.tid, time_index, interval))
            avg_switch_time += interval
            cnt += 1
            if min_time == -1:
                min_time = interval
            else:
                min_time = min(min_time, interval)
            max_time = max(max_time, interval)
        if cnt == 0:
            return
        avg_switch_time = float(avg_switch_time) / cnt
        print "<{0}> Average switch time {1:0.1f}, [{2}, {3}]".format(\
              self.tid, avg_switch_time, min_time, max_time)

    def write_connect_time_csv(self, fcsv):
        avg_connect_time, min_time, max_time = 0, -1, 0
        cnt = 0
        for time_index, interval in self.connect_time:
            time_index = self.format_time_index(time_index)
            interval = self.format_interval(interval)
            fcsv.write("{0},{1},{2}\n".format(self.tid, time_index, interval))
            avg_connect_time += interval
            cnt += 1
            if min_time == -1:
                min_time = interval
            else:
                min_time = min(min_time, interval)
            max_time = max(max_time, interval)
        if cnt == 0:
            return
        avg_connect_time = float(avg_connect_time) / cnt
        print "<{0}> Average connect time {1:0.1f}, count {2}, [{3}, {4}]".format(\
              self.tid, avg_connect_time, cnt, min_time, max_time)

def get_ts(s):
    ts = datetime.datetime.strptime(s, "%H:%M:%S.%f")
    return ts

def need_this_line(line):
    toks = line.split()
    if len(toks) == 0:
        return False
    s = toks[0]
    try:
        ts = get_ts(s)
    except:
        return False
    return True

def extract_pid(line):
    s = line.split()[1]
    l = s.index("<")
    r = s.index(">")
    return s[l+1:r]

def read_post_log(flog):
    threads = {}

    flog.seek(0, 2)
    fsize = flog.tell()
    flog.seek(0, 0)

    next_level = 10
    while True:
        line = flog.readline()
        if line == "":
            break
        percent = flog.tell()/float(fsize) * 100
        if percent >= next_level:
            print "{0:0.1f}%".format(percent)
            next_level += 10
            if next_level > 100:
                next_level = 100
        if not need_this_line(line):
            continue
        pid = extract_pid(line)
        if pid not in threads:
            threads[pid] = SendThread(pid)
        threads[pid].consume_line(line)

    return threads

def write_thread_csv(threads):
    fcsv_send = open("post_send_time.csv", "w")
    fcsv_switch = open("post_switch_time.csv", "w")
    fcsv_connect = open("post_connect_time.csv", "w")
    for thread in threads.values():
        thread.write_send_time_csv(fcsv_send)
    for thread in threads.values():
        thread.write_switch_time_csv(fcsv_switch)
    for thread in threads.values():
        thread.write_connect_time_csv(fcsv_connect)
    fcsv_send.close()
    fcsv_switch.close()
    fcsv_connect.close()

_SPOILER_SEND_THREASHOLD_ = 200

flog = open("post.log", "r")
threads = read_post_log(flog)
flog.close()
write_thread_csv(threads)
