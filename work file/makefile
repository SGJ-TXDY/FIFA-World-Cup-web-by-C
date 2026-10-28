# 世界杯球员信息查询服务器
TARGET:=worldcup_server

OBJS+=webserver.o
OBJS+=user.o
OBJS+=db.o
OBJS+=player.o
OBJS+=sqlite3.o

# sqlite3.c 不在本目录，取自参考项目 webserver_wordmean
SQLITE_DIR:=../webserver_wordmean

CC:=gcc
CFLAGS:=-Wall -g
LDFLAGS:=-lpthread -ldl

$(TARGET):$(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

# 本项目源文件：head.h 改动后自动重新编译
%.o:%.c head.h
	$(CC) $(CFLAGS) -c $< -o $@

# sqlite3.c 体积大、编译慢，单独规则编译（不开 -Wall，避免大量无关告警）
sqlite3.o:$(SQLITE_DIR)/sqlite3.c
	$(CC) -O2 -c $< -o $@

run:$(TARGET)
	./$(TARGET)

# clean 保留 sqlite3.o，distclean 连同它一起删除
.PHONY:run clean distclean
clean:
	rm -f $(TARGET) a.out $(filter-out sqlite3.o,$(OBJS))

distclean:clean
	rm -f sqlite3.o
