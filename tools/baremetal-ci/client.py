import argparse
import os
import time
import requests
from colorama import Fore, Back, Style
from dotenv import load_dotenv

api = None

class API:
    def __init__(self, url, api_key):
        self.url = url
        self.api_key = api_key

    def request(self, method, path, **kwargs):
        kwargs.setdefault("headers", {})
        kwargs["headers"]["Authorization"] = f"bearer {self.api_key}"
        resp = requests.request(method, self.url + path, **kwargs)
        resp.raise_for_status()
        return resp

    def get(self, path, **kwargs):
        return self.request("get", path, **kwargs)

    def post(self, path, **kwargs):
        return self.request("post", path, **kwargs)

    def put(self, path, **kwargs):
        return self.request("put", path, **kwargs)

def create_build(title, created_by, machine, image):
    form = {
        "title": title,
        "machine": machine,
        "created_by" : created_by,
    }

    files = {
        "image": open(image, "rb"),
    }
    resp = api.post("/api/builds", data=form, files=files)
    return resp.json()["id"]

def get_run_for_build(build_id):
    runs = api.get(f"/api/builds/{build_id}/runs").json()
    if len(runs) == 0:
        return None
    return runs[0]

def cprint(color, message):
    print(f"{Style.BRIGHT}{color}==> {Fore.RESET}BareMetal CI: {message}{Style.RESET_ALL}")

def run_command(args):
    cprint(Fore.BLUE, "Enqueueing a build...")
    build_id = create_build(args.title, args.created_by, args.machine, args.image)
    for _ in range(0, args.timeout):
        run = get_run_for_build(build_id)
        if run is not None:
            run_id = run["id"]
            break
        time.sleep(args.polling_interval)
    else:
        cprint(Fore.RED, "No runners accpeted the submitted build")
        return

    cprint(Fore.BLUE, f"Runner {run.runner_name}: {api.url}/runs/{run_id}")
    for _ in range(0, args.timeout):
        cprint(Fore.BLUE, "Running on a machine...")
        if run["status"] in ["failure", "success"]:
            break
        time.sleep(args.polling_interval)
    else:
        cprint(Fore.RED, "machine timed out")
        api.put(f"/api/runs/{run_id}", json={ "status": "timeout" })
        return

    if build["status"] == "success":
        cprint(Fore.GREEN, "successfully finished tests")
    else:
        cprint(Fore.YELLOW, f"finished with {build['status']}")

def main():
    global api
    load_dotenv()
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(required=True)
    parser.add_argument("--url", required=True, help="The BareMetal CI Server URL.")
    parser.add_argument("--api-key", help="The API Key.")
    run_parser = subparsers.add_parser("run", help="Run an image on a CI runner.")
    run_parser.add_argument("image", help="The executable kernel image (resea.elf).")
    run_parser.add_argument("--polling-interval", type=int, default=1)
    run_parser.add_argument("--timeout", type=int, default=15)
    run_parser.add_argument("--title", required=True)
    run_parser.add_argument("--machine", required=True)
    run_parser.add_argument("--created-by", required=True)
    run_parser.set_defaults(func=run_command)
    args = parser.parse_args()

    api_key = os.environ.get("BAREMETALCI_API_KEY", args.api_key)
    api = API(args.url, api_key)

    args.func(args)

if __name__ == "__main__":
    main()
