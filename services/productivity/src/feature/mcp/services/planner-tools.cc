#include "planner-tools.hxx"

#include "tool-support.hxx"

#include <feature/project-task/dtos/create-project-task-dto.hxx>
#include <feature/project-task/dtos/update-project-task-dto.hxx>
#include <feature/project-task/services/project-task-feature-service.hxx>
#include <feature/project/dtos/create-project-dto.hxx>
#include <feature/project/services/project-feature-service.hxx>
#include <shared/repositories/project-task/project-task-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <string_view>

namespace productivity_tools
{

namespace
{
constexpr int kDefaultListed = 10;
constexpr int kMostListed = 20;
constexpr std::array<std::string_view, 2> kClosedProjects{"done", "canceled"};
constexpr std::array<std::string_view, 2> kClosedTasks{"done", "canceled"};

std::string lang(const argus::mcp::ToolInvocation& invocation)
{
  return std::string(argus::mcp::speech::languageOf(invocation));
}

bool closed(const std::string& status, const std::array<std::string_view, 2>& closedStatuses)
{
  return std::ranges::find(closedStatuses, status) != closedStatuses.end();
}

size_t limitOf(const Json::Value& arguments)
{
  if (!arguments["limit"].isIntegral())
    return static_cast<size_t>(kDefaultListed);
  return static_cast<size_t>(std::clamp(arguments["limit"].asInt(), 1, kMostListed));
}

std::vector<std::string> namesOf(const std::vector<ProjectSchema>& projects)
{
  std::vector<std::string> names;
  names.reserve(projects.size());
  for (const auto& project : projects)
    names.push_back(project.name);
  return names;
}

argus::mcp::ToolOutcome withProjects(argus::mcp::ToolOutcome outcome, const std::vector<std::string>& names)
{
  Json::Value listed(Json::arrayValue);
  for (const std::string& name : names)
    listed.append(name);
  outcome.structured["projects"] = std::move(listed);
  return outcome;
}

struct ChosenProject
{
  std::optional<ProjectSchema> project;
  argus::mcp::ToolOutcome failure;
};

drogon::Task<ChosenProject> chooseProject(const argus::mcp::ToolInvocation& invocation)
{
  const ProjectRepository repository;
  const auto projects = co_await repository.findByOwner(invocation.caller.userId);
  const std::string asked = invocation.arguments.get("project", "").asString();
  if (invocation.arguments["project_id"].isIntegral()) {
    const auto found = std::ranges::find(projects, invocation.arguments["project_id"].asInt64(), &ProjectSchema::id);
    if (found != projects.end())
      co_return ChosenProject{.project = *found, .failure = {}};
  }
  const auto names = namesOf(projects);
  if (!asked.empty()) {
    const auto match = text_norm::matchName(names, asked);
    if (match.kind == text_norm::NameMatchKind::Exact)
      co_return ChosenProject{.project = projects.at(match.hits.front()), .failure = {}};
    if (match.kind == text_norm::NameMatchKind::Ambiguous) {
      std::vector<std::string> options;
      options.reserve(match.hits.size());
      for (const size_t index : match.hits)
        options.push_back(projects.at(index).name);
      co_return ChosenProject{.project = std::nullopt,
                              .failure = withProjects(argus::mcp::speech::refuse({.invocation = invocation,
                                                  .spanish = "¿En cuál proyecto? " + argus::mcp::speech::joined(options, "es") + ".",
                                                  .english = "Which project? " + argus::mcp::speech::joined(options, "en") + "."},
                                                 "ambiguous_project"),
                                                      options)};
    }
    co_return ChosenProject{.project = std::nullopt,
                            .failure = withProjects(argus::mcp::speech::refuse({.invocation = invocation,
                                                .spanish = "No encuentro un proyecto llamado " + asked + ". Tus proyectos son: " +
                                                           (names.empty() ? std::string("ninguno") : argus::mcp::speech::joined(names, "es")) + ".",
                                                .english = "I cannot find a project called " + asked + ". Your projects are: " +
                                                           (names.empty() ? std::string("none") : argus::mcp::speech::joined(names, "en")) + "."},
                                               "unknown_project"),
                                                    names)};
  }
  std::vector<ProjectSchema> open;
  std::ranges::copy_if(projects, std::back_inserter(open),
                       [](const ProjectSchema& project) { return !closed(project.status, kClosedProjects); });
  if (open.size() == 1)
    co_return ChosenProject{.project = open.front(), .failure = {}};
  if (open.empty())
    co_return ChosenProject{.project = std::nullopt,
                            .failure = argus::mcp::speech::refuse({.invocation = invocation,
                                                .spanish = "Todavía no tienes proyectos. Primero crea uno y luego le agrego la tarea.",
                                                .english = "You have no projects yet. Create one first and then I will add the task."},
                                               "no_projects")};
  co_return ChosenProject{.project = std::nullopt,
                          .failure = withProjects(argus::mcp::speech::refuse({.invocation = invocation,
                                              .spanish = "¿En cuál proyecto va? " + argus::mcp::speech::joined(namesOf(open), "es") + ".",
                                              .english = "Which project is it for? " + argus::mcp::speech::joined(namesOf(open), "en") + "."},
                                             "project_needed"),
                                                  namesOf(open))};
}

Json::Value projectSummary(const ProjectSchema& project)
{
  Json::Value out(Json::objectValue);
  out["id"] = project.id;
  out["name"] = project.name;
  out["status"] = project.status;
  if (project.targetAt)
    out["targetAt"] = *project.targetAt;
  return out;
}

Json::Value taskSummary(const ProjectTaskSchema& task, const std::string& project)
{
  Json::Value out(Json::objectValue);
  out["id"] = task.id;
  out["title"] = task.title;
  out["status"] = task.status;
  out["priority"] = task.priority;
  out["project"] = project;
  if (task.dueAt)
    out["dueAt"] = *task.dueAt;
  return out;
}

std::string describeTask(const ProjectTaskSchema& task, const std::string& project, const argus::mcp::ToolInvocation& invocation)
{
  std::string out = "«" + task.title + "» (" + project + ")";
  if (task.dueAt)
    out += (argus::mcp::speech::inEnglish(invocation) ? ", due " : ", para ") + spokenWhen({.epoch = *task.dueAt, .allDay = true}, lang(invocation));
  return out;
}

struct OpenTask
{
  ProjectTaskSchema task;
  std::string project;
};

drogon::Task<std::vector<OpenTask>> openTasks(const argus::mcp::ToolInvocation& invocation, std::optional<int64_t> only)
{
  const ProjectRepository projects;
  const ProjectTaskRepository tasks;
  std::vector<OpenTask> out;
  for (const auto& project : co_await projects.findByOwner(invocation.caller.userId)) {
    if (only && *only != project.id)
      continue;
    for (auto& task : co_await tasks.findByProject(project.id))
      if (!closed(task.status, kClosedTasks))
        out.push_back({.task = std::move(task), .project = project.name});
  }
  co_return out;
}
}

drogon::Task<argus::mcp::ToolOutcome> createProject(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const Json::Value& arguments = invocation.arguments;
  Json::Value body(Json::objectValue);
  body["name"] = arguments.get("name", "").asString();
  body["description"] = arguments.get("description", "").asString();
  if (const auto targetAt = timeArgument(arguments, "target_at"))
    body["targetAt"] = *targetAt;
  ProjectSchema row;
  try {
    const auto dto = CreateProjectDto::fromJson(body);
    const ProjectFeatureService service;
    row = co_await service.create({.body = dto,
                                   .ownerId = userId,
                                   .idempotencyKey = idempotencyKey({.scope = "project.create",
                                                                     .userId = userId,
                                                                     .subject = dto.name,
                                                                     .at = 0})});
  }
  catch (const ValidationException& error) {
    co_return invalid(error, invocation);
  }
  argus::mcp::ToolOutcome outcome;
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Project created: " : "Proyecto creado: ") + row.name + ".";
  outcome.structured = projectSummary(row);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> listProjects(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const ProjectRepository repository;
  const std::string status = invocation.arguments.get("status", "").asString();
  std::vector<ProjectSchema> shown;
  for (auto& project : co_await repository.findByOwner(userId))
    if (status.empty() ? !closed(project.status, kClosedProjects) : project.status == status)
      shown.push_back(std::move(project));
  argus::mcp::ToolOutcome outcome;
  Json::Value listed(Json::arrayValue);
  if (shown.empty()) {
    outcome.text = argus::mcp::speech::say({.invocation = invocation, .spanish = "No tienes proyectos.", .english = "You have no projects."});
    outcome.structured["projects"] = std::move(listed);
    co_return outcome;
  }
  const size_t limit = limitOf(invocation.arguments);
  std::vector<std::string> spoken;
  for (size_t index = 0; index < shown.size() && index < limit; ++index) {
    spoken.push_back(shown[index].name + " (" + shown[index].status + ")");
    listed.append(projectSummary(shown[index]));
  }
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Your projects: " : "Tus proyectos: ") + argus::mcp::speech::joined(spoken, lang(invocation)) + ".";
  outcome.structured["projects"] = std::move(listed);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> createTask(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const auto chosen = co_await chooseProject(invocation);
  if (!chosen.project)
    co_return chosen.failure;
  const Json::Value& arguments = invocation.arguments;
  Json::Value body(Json::objectValue);
  body["projectId"] = chosen.project->id;
  body["title"] = arguments.get("title", "").asString();
  body["priority"] = arguments.get("priority", "none").asString();
  if (const auto dueAt = timeArgument(arguments, "due_at"))
    body["dueAt"] = *dueAt;
  std::optional<ProjectTaskSchema> row;
  try {
    const auto dto = CreateProjectTaskDto::fromJson(body);
    const ProjectTaskFeatureService service;
    row = co_await service.create({.body = dto,
                                   .actorId = userId,
                                   .idempotencyKey = idempotencyKey({.scope = "task.create",
                                                                     .userId = userId,
                                                                     .subject = dto.title,
                                                                     .at = dto.projectId})});
  }
  catch (const ValidationException& error) {
    co_return invalid(error, invocation);
  }
  if (!row)
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "No puedes agregar tareas a ese proyecto.",
                       .english = "You cannot add tasks to that project."},
                      "forbidden");
  argus::mcp::ToolOutcome outcome;
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Task added: " : "Tarea agregada: ") + describeTask(*row, chosen.project->name, invocation) + ".";
  outcome.structured = taskSummary(*row, chosen.project->name);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> listTasks(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  std::optional<int64_t> only;
  if (!invocation.arguments.get("project", "").asString().empty() || invocation.arguments["project_id"].isIntegral()) {
    const auto chosen = co_await chooseProject(invocation);
    if (!chosen.project)
      co_return chosen.failure;
    only = chosen.project->id;
  }
  const auto tasks = co_await openTasks(invocation, only);
  argus::mcp::ToolOutcome outcome;
  Json::Value listed(Json::arrayValue);
  if (tasks.empty()) {
    outcome.text = argus::mcp::speech::say({.invocation = invocation, .spanish = "No tienes tareas pendientes.", .english = "You have no pending tasks."});
    outcome.structured["tasks"] = std::move(listed);
    co_return outcome;
  }
  const size_t limit = limitOf(invocation.arguments);
  std::vector<std::string> spoken;
  for (size_t index = 0; index < tasks.size() && index < limit; ++index) {
    spoken.push_back(describeTask(tasks[index].task, tasks[index].project, invocation));
    listed.append(taskSummary(tasks[index].task, tasks[index].project));
  }
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Pending tasks: " : "Tareas pendientes: ") + argus::mcp::speech::joined(spoken, lang(invocation)) + ".";
  if (tasks.size() > limit)
    outcome.text += argus::mcp::speech::inEnglish(invocation) ? " And " + std::to_string(tasks.size() - limit) + " more."
                                        : " Y " + std::to_string(tasks.size() - limit) + " más.";
  outcome.structured["tasks"] = std::move(listed);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> completeTask(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const auto tasks = co_await openTasks(invocation, std::nullopt);
  std::optional<OpenTask> target;
  if (invocation.arguments["task_id"].isIntegral()) {
    const auto found = std::ranges::find(tasks, invocation.arguments["task_id"].asInt64(),
                                         [](const OpenTask& open) { return open.task.id; });
    if (found != tasks.end())
      target = *found;
  }
  else {
    const std::string asked = invocation.arguments.get("title", "").asString();
    std::vector<std::string> titles;
    titles.reserve(tasks.size());
    for (const auto& open : tasks)
      titles.push_back(open.task.title);
    const auto match = text_norm::matchName(titles, asked);
    if (match.kind == text_norm::NameMatchKind::Ambiguous) {
      std::vector<std::string> options;
      options.reserve(match.hits.size());
      for (const size_t index : match.hits)
        options.push_back(describeTask(tasks.at(index).task, tasks.at(index).project, invocation));
      co_return argus::mcp::speech::refuse({.invocation = invocation,
                         .spanish = "¿Cuál de estas tareas? " + argus::mcp::speech::joined(options, "es") + ".",
                         .english = "Which of these tasks? " + argus::mcp::speech::joined(options, "en") + "."},
                        "ambiguous_task");
    }
    if (match.kind == text_norm::NameMatchKind::Exact)
      target = tasks.at(match.hits.front());
  }
  if (!target)
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "No encuentro esa tarea entre las pendientes.",
                       .english = "I cannot find that task among the pending ones."},
                      "task_not_found");
  Json::Value body(Json::objectValue);
  body["status"] = "done";
  std::optional<ProjectTaskSchema> row;
  try {
    const auto dto = UpdateProjectTaskDto::fromJson(body);
    const ProjectTaskFeatureService service;
    row = co_await service.update({.id = target->task.id, .body = dto, .actorId = userId});
  }
  catch (const ValidationException& error) {
    co_return invalid(error, invocation);
  }
  if (!row)
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "No pude marcar esa tarea como hecha.",
                       .english = "I could not mark that task as done."},
                      "forbidden");
  argus::mcp::ToolOutcome outcome;
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Done: " : "Hecho: ") + describeTask(*row, target->project, invocation) + ".";
  outcome.structured = taskSummary(*row, target->project);
  co_return outcome;
}

}
