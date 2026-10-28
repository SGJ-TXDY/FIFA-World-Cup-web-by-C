OBJ:=a.out

OBJS+=webserver.o
OBJS+=sqlite3.o

LDFLAGS:=-lpthread -ldl

$(OBJ):$(OBJS)
	gcc $^ -o $@ $(LDFLAGS)
%.o:%.c 
	gcc -c $^ -o $@

.PHONY:
clean:
	rm $(OBJ)