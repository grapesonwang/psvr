CC=mpicxx

# C-Compiler flags
CFLAGS=-O3 -Wall

# linker
LD=mpicxx
LFLAGS=-O3 -Wall

all: svr_train svr_predict

clean:
	rm -f *.o
	rm -f svr_train svr_predict

io.o: io.cc io.h
	$(CC) -c $(CFLAGS)  io.cc -o io.o

util.o: util.cc util.h
	$(CC) -c $(CFLAGS)  util.cc -o util.o

timer.o: timer.cc timer.h
	$(CC) -c $(CFLAGS)  timer.cc -o timer.o

document.o: document.cc document.h
	$(CC) -c $(CFLAGS)  document.cc -o document.o

kernel.o: kernel.cc kernel.h
	$(CC) -c $(CFLAGS)  kernel.cc -o kernel.o

model.o: model.cc model.h
	$(CC) -c $(CFLAGS)  model.cc -o model.o

matrix.o: matrix.cc matrix.h
	$(CC) -c $(CFLAGS)  matrix.cc -o matrix.o

matrix_manipulation.o: matrix_manipulation.cc matrix_manipulation.h
	$(CC) -c $(CFLAGS)  matrix_manipulation.cc -o matrix_manipulation.o

parallel_interface.o: parallel_interface.cc parallel_interface.h
	$(CC) -c $(CFLAGS)  parallel_interface.cc -o parallel_interface.o

pd_ipm.o: pd_ipm.cc pd_ipm.h
	$(CC) -c $(CFLAGS)  pd_ipm.cc -o pd_ipm.o

svr_train.o: svr_train.cc
	$(CC) -c $(CFLAGS)  svr_train.cc -o svr_train.o

svr_predict.o: svr_predict.cc svr_predict.h
	$(CC) -c $(CFLAGS)  svr_predict.cc -o svr_predict.o


svr_train: timer.o io.o parallel_interface.o util.o model.o document.o matrix.o kernel.o matrix_manipulation.o pd_ipm.o svr_train.o
	$(LD) $(LFLAGS) timer.o io.o parallel_interface.o util.o document.o model.o matrix.o kernel.o matrix_manipulation.o pd_ipm.o svr_train.o -o svr_train

svr_predict: svr_predict.o model.o document.o parallel_interface.o
	$(LD) $(LFLAGS) timer.o io.o util.o document.o model.o kernel.o parallel_interface.o svr_predict.o -o svr_predict
