class AppRunner
{
public:
  AppRunner() {}
};

class SharedBoot
{
public:
  SharedBoot() { runner_.emplace(); }

private:
  std::optional<AppRunner> runner_;
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}
