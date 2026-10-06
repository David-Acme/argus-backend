class AppRunner
{
public:
  AppRunner() {}
};

void boot()
{
  static const auto runner = [] {
    configure();
    return std::make_unique<AppRunner>();
  }();
}
