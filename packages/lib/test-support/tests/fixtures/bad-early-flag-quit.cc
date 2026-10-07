class Runner
{
public:
  ~Runner()
  {
    if (drogon::app().isRunning()) {
      drogon::app().quit();
      runner_.join();
    }
  }

private:
  std::thread runner_;
};
