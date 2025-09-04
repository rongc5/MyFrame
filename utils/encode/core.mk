HOME_DIR=./
SRC_DIR=${HOME_DIR}
BUILD_DIR=${HOME_DIR}core
OBJ_DIR=${BUILD_DIR}

INCLUDE = -I./
CPPFLAGS = -g -Wall $(INCLUDE) -O2 -std=c++11
CC = g++ $(CPPFLAGS)

OBJ = ${OBJ_DIR}/des.o \
      ${OBJ_DIR}/uconv.o

AR = ar
LIBNAME = encode

all: init $(LIBNAME)

init:
	mkdir -p $(BUILD_DIR) lib

clean:
	/bin/rm -rf $(BUILD_DIR) lib/lib$(LIBNAME).a

$(LIBNAME): $(OBJ)
	$(AR) -ruv $(BUILD_DIR)/lib$@.a $(OBJ)

${OBJ_DIR}/%.o : ${SRC_DIR}/%.cpp
	$(CC) $(CXXFLAGS) -c $< -o $@ $(INCLUDE)