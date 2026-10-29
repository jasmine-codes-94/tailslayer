CXX = g++
CXXFLAGS = -O2 -std=c++17 -pthread -D_GNU_SOURCE
LDFLAGS = -pthread

OBJS = claim_race.o benchmark.o stats.o

TARGET = claim_race

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

claim_race.o: claim_race.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

benchmark.o: ../benchmark/benchmark.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

stats.o: ../benchmark/stats.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
