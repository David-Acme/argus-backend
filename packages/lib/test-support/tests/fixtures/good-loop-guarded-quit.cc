class Runner
{
public:
  ~Runner()
  {
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
    }
  }

private:
  std::thread runner_;
};
