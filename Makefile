# ──────────────────────────────────────────────────────
# 弥生会計バックアップ KB12→KB26 コンバータ Makefile
# ──────────────────────────────────────────────────────

CXX      ?= g++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
TARGET    = kb_converter
SRCDIR    = src
SRCS      = $(SRCDIR)/main.cpp $(SRCDIR)/kb_converter.cpp
OBJS      = $(SRCS:.cpp=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
