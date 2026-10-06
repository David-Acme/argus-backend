class AppRunner
{
public:
  AppRunner() {}
};

void run()
{
  AppRunner runner;
  static int counter = 0;
  ++counter;
}
