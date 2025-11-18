from pickle import dump

with open("turbacz/data/cookies.pickle", "wb") as cookies:
    dump({}, cookies)
