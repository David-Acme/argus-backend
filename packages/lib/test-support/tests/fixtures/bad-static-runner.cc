class AppRunner
{
public:
  AppRunner() {}
};

void boot()
{
  static AppRunner runner;
}
